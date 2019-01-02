#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/videodev2.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <asm/atomic.h>
#include <asm/unaligned.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf-core.h>

#include "uvcvideo.h"

/* ����һ��usb_device �ṹ�� */
static struct usb_device *my_usb_device;

/* ������������� */
static int uv_ctrl_interface;

/* ��Ƶ����������� */
static int uv_streaming_interface;

/* ����һ��vodeo_device�ṹ�� */
static struct video_device *my_video_device; 

/* ����һ��v4l2_formt  �ṹ�� */
static struct v4l2_format my_v4l2_format;

/* ���ػ��� */
static struct usb_v4l2_buff 
{
   struct v4l2_buffer buffer;
   int buff_state;
   int mmap_state;
   wait_queue_head_t pocs_wait;  /* ��ѯ���ݣ��������߶��� */

   struct list_head driver;
   struct list_head user;   
}usb_v4l2_buff;

/* ���ض��� */
static struct usb_v4l2_queue 
{
   void *start_addr;
   int count;
   int buf_size;
   struct usb_v4l2_buff buffers[32];

   struct list_head usr_buf_queue;
   struct list_head ker_buf_queue;

   struct urb *urb[32];
   char* urb_buff[32];
   dma_addr_t urb_dma[32];
   unsigned int urb_size;
   
   
}usb_v4l2_queue;

/* ��Ƶ������ */
struct usb_v4l2_streaming_control {
	__u16 bmHint;
	__u8  bFormatIndex;   /* ��Ƶ��ʽ����     */
	__u8  bFrameIndex;    /* ��Ƶ֡���� */
	__u32 dwFrameInterval; /* ��Ƶ֡��� */
	__u16 wKeyFrameRate;
	__u16 wPFrameRate;
	__u16 wCompQuality;
	__u16 wCompWindowSize;
	__u16 wDelay;
	__u32 dwMaxVideoFrameSize;       /* �����Ƶ֡��С */
	__u32 dwMaxPayloadTransferSize;  /*  */
	__u32 dwClockFrequency;      /* ʱ��Ƶ�� */
	__u8  bmFramingInfo;
	__u8  bPreferedVersion;
	__u8  bMinVersion;
	__u8  bMaxVersion;
}my_usb_v4l2_streaming_control;

/* Values for bmHeaderInfo (Video and Still Image Payload Headers, 2.4.3.3) */
#define UVC_STREAM_EOH	(1 << 7)
#define UVC_STREAM_ERR	(1 << 6)
#define UVC_STREAM_STI	(1 << 5)
#define UVC_STREAM_RES	(1 << 4)
#define UVC_STREAM_SCR	(1 << 3)
#define UVC_STREAM_PTS	(1 << 2)
#define UVC_STREAM_EOF	(1 << 1)
#define UVC_STREAM_FID	(1 << 0)

#define ICE_P(x) (sizeof(int) == sizeof(*(1 ? ((void*)((x) * 0l)) : (int*)1)))


/**********************************v4l2_ioctl_ops*****************************************/

static int my_v4l2_query_cap(struct file *file, void *fh, struct v4l2_capability *cap)
{
    memset(cap, 0,sizeof(*cap));
	
  	/* ����v4l2_capability�ṹ��      */
    strcpy(cap->driver, "usb_v4l2_camera");
    strcpy(cap->card,"usb_v4l2_camera");
    cap->version = 1;
	cap->capabilities = V4L2_CAP_VBI_CAPTURE | V4L2_CAP_STREAMING ;
	return 0;
}

static int my_v4l2_enum_fmt_vid_cap(struct file *file, void *fh,struct v4l2_fmtdesc *fmtdesc)
{
   /*�ж�֧�ָ�ʽ*/
   if(fmtdesc->index >= 1) return -EINVAL;
   
   /* ����v4l2_fmtdesc�ṹ�� */
   strcpy(fmtdesc->description,"4:2:2, packed, YUYV");
          fmtdesc->pixelformat = V4L2_PIX_FMT_YUYV;
   
   return 0;
}

static int my_v4l2_g_fmt_vid_cap(struct file *file, void *fh,struct v4l2_format *fmt)
{
   /* Ӧ�ó���õ���ʽ */
   memcpy(fmt,&my_v4l2_format,sizeof(my_v4l2_format)); 
   return 0;
}

static int my_v4l2_try_fmt_vid_cap(struct file *file, void *fh,struct v4l2_format *fmt)
{
    /* �жϸ�ʽ�������Ƿ���ȷ */
    if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)     return -EINVAL;
    if (fmt->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)return -EINVAL;

    /* ����һ֡����ȣ��߶� */
    fmt->fmt.pix.width = 352;                                                    /* ֡�ֽڿ�� */
	fmt->fmt.pix.height = 288;    											     /* ֡�ֽڸ߶� */
	fmt->fmt.pix.bytesperline = fmt->fmt.pix.width * 16 ;                        /* ֡���ؿ�� */
	fmt->fmt.pix.sizeimage = fmt->fmt.pix.bytesperline * fmt->fmt.pix.height ;   /* ֡���ش�С */

	return 0;
}

static int my_v4l2_s_fmt_vid_cap(struct file *file, void *fh,struct v4l2_format *fmt)
{
    int ret = 0 ;
    if( (ret = my_v4l2_s_fmt_vid_cap(file,NULL,fmt)))
    	{
         memcpy(&my_v4l2_format, fmt , sizeof(my_v4l2_format));
         return 0;
    	}
	return ret;

}

static int free_usb_v4l2_queue(void)
{
   if(usb_v4l2_queue.start_addr)
   	{
      vfree(usb_v4l2_queue.start_addr);
      memset(&usb_v4l2_queue,0,sizeof(usb_v4l2_queue));
      usb_v4l2_queue.start_addr =NULL;
   }
   return 0;
}

static int my_v4l2_reqbufs(struct file *file, void *fh, struct v4l2_requestbuffers *reqbuf)
{
   /* ��Ӧ�ó����֪��Ҫ�������Ĵ�С */
   int frame_num = reqbuf->count;
   /* һ֡��С */
   int frame_size = PAGE_ALIGN(my_v4l2_format.fmt.pix.sizeimage);
   int var=0;
   /* ������ʼ��ַ */
   void *mem = NULL;

   /* ��Ҫ�ļ�� */
   if(free_usb_v4l2_queue() < 0) return -1;
   if(frame_num == 0) return -1;

   

   /* Ϊ����֡����32λ�������û��ռ仺�� */
   for( ; frame_num>0 ; frame_num--)
   	{
      mem = vmalloc_32(var * frame_size);
	  if(mem != NULL)  break;
   }

   if(mem == NULL)  return ENOMEM;

   /*��ʼ���������*/
   memset(&usb_v4l2_queue, 0, sizeof(usb_v4l2_queue));   
   INIT_LIST_HEAD(&usb_v4l2_queue.usr_buf_queue);
   INIT_LIST_HEAD(&usb_v4l2_queue.ker_buf_queue);

   /* ���������ÿһ������ */
   for(var=0; var<frame_num; var++)
   	{
      usb_v4l2_queue.buffers[var].buffer.index = var;
	  usb_v4l2_queue.buffers[var].buffer.m.offset = var*frame_size;
      usb_v4l2_queue.buffers[var].buffer.length = my_v4l2_format.fmt.pix.sizeimage;
      usb_v4l2_queue.buffers[var].buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	  usb_v4l2_queue.buffers[var].buffer.sequence = 0;
	  usb_v4l2_queue.buffers[var].buffer.field = V4L2_FIELD_NONE;
	  usb_v4l2_queue.buffers[var].buffer.memory = V4L2_MEMORY_MMAP;
	  usb_v4l2_queue.buffers[var].buffer.flags = 0;
	  usb_v4l2_queue.buffers[var].buff_state = VIDEOBUF_IDLE;
	  init_waitqueue_head(&usb_v4l2_queue.buffers[var].pocs_wait);
   }

   /* ������������ */
   usb_v4l2_queue.start_addr = mem;
   usb_v4l2_queue.count = frame_num;
   usb_v4l2_queue.buf_size = frame_size;

   return frame_num;
}

static int my_v4l2_querybuf(struct file *file, void *fh, struct v4l2_buffer *v4l2_buf)
{
    /* �ж�Ӧ�ó�������Ƿ���ȷ */
    if(v4l2_buf->index > usb_v4l2_queue.count ) return -EINVAL;

    /* �ӱ��ض��и���Ӧ�ó���ȡ����Ӧ�Ļ��� */
	memcpy(v4l2_buf,&usb_v4l2_queue.buffers[v4l2_buf->index].buffer ,sizeof(*v4l2_buf));

	/* ����usb_v4l2_queue.usb_v4l2_buffers״̬����v4l2_buf��־λ */
    if(usb_v4l2_queue.buffers[v4l2_buf->index].mmap_state)
		 v4l2_buf->flags |= V4L2_BUF_FLAG_MAPPED;
	
	switch(usb_v4l2_queue.buffers[v4l2_buf->index].buff_state)
		{
           case VIDEOBUF_ERROR :
		   case VIDEOBUF_DONE :  v4l2_buf->flags |= V4L2_BUF_FLAG_DONE ;  break ;
           case VIDEOBUF_QUEUED: 
		   case VIDEOBUF_ACTIVE : v4l2_buf->flags |= V4L2_BUF_FLAG_QUEUED; break;
           case VIDEOBUF_IDLE :
           default : break;
	    }
	return 0;
}

static int my_v4l2_qbuf(struct file *file, void *fh, struct v4l2_buffer *v4l2_buff)
{
   struct usb_v4l2_buff *temp_usb_v4l2_buff;

   /* ��⴫�� */
   if(v4l2_buff->index > usb_v4l2_queue.count || v4l2_buff->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || v4l2_buff->memory != V4L2_MEMORY_MMAP )
      return -EINVAL;
   
   /* �޸Ļ���״̬ */
   temp_usb_v4l2_buff = &usb_v4l2_queue.buffers[v4l2_buff->index]; 
   if(temp_usb_v4l2_buff->buff_state != VIDEOBUF_IDLE)  return -EINVAL;
   
   /* ������� */
   list_add(&temp_usb_v4l2_buff->user, &usb_v4l2_queue.usr_buf_queue);
   list_add(&temp_usb_v4l2_buff->driver, &usb_v4l2_queue.ker_buf_queue);

   return 0;
}

static int my_v4l2_dqbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
    struct usb_v4l2_buff *temp_usb_v4l2_buff;

    /* �ж϶����Ƿ�Ϊ�� */
    if(list_empty(&usb_v4l2_queue.usr_buf_queue)) return -EINVAL;

    /* �Ӷ���ȡ�������� */
    temp_usb_v4l2_buff = list_first_entry(&usb_v4l2_queue.usr_buf_queue,struct usb_v4l2_buff,user);

	/* �޸Ļ�����״̬ */
    switch (temp_usb_v4l2_buff->buff_state)
    	{
          case VIDEOBUF_ERROR:return  -EIO;
		  case VIDEOBUF_DONE:{ temp_usb_v4l2_buff->buff_state = VIDEOBUF_IDLE;  break; }
		  case VIDEOBUF_IDLE:
		  case VIDEOBUF_QUEUED:
		  case VIDEOBUF_ACTIVE:
		  default: 
		          return -EINVAL; 
    	}
	list_del(&temp_usb_v4l2_buff->user);
    return 0;
}

/***********************************streaming_params******************************************/
static int usb_v4l2_try_streaming_params(struct usb_v4l2_streaming_control *ctrl)
{
    unsigned char *data;
    unsigned short size = 0 ;
    unsigned char type = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    unsigned int pipe;
    int ret = 0;

    /* ��usb_v4l2_streaming_control��Աȫ������ ,��һЩ��ʼ��*/
    memset(ctrl, 0 , sizeof(*ctrl));
    ctrl->bmHint = 1;
    ctrl->bFormatIndex = 1;
    ctrl->bFrameIndex = 2;
    ctrl->dwFrameInterval = 333333;   /* ֡���ʱ��==>31fps */
   
    /* ����  �������ݴ�С   */
    size = 0x0100 > 0x0110 ? 34 : 26;

    /* ���ùܵ�pipe  */
    pipe = (SET_CUR & 0x80) ? usb_rcvctrlpipe(my_usb_device, 0) : usb_sndctrlpipe(my_usb_device, 0);

     /* ������������   */
    type |= (SET_CUR & 0x80) ? USB_DIR_IN : USB_DIR_OUT;

    /* ����	usb_v4l2_streaming_control ����Ҫ�����ֵ  */
    data = kzalloc(size, GFP_KERNEL);
    if(data == NULL) return -ENOMEM;
   
    *(__le16 *)&data[0] = cpu_to_le16(ctrl->bmHint);
    data[2] = ctrl->bFormatIndex;
    data[3] = ctrl->bFrameIndex;
    *(__le32 *)&data[4] = cpu_to_le32(ctrl->dwFrameInterval);
    *(__le16 *)&data[8] = cpu_to_le16(ctrl->wKeyFrameRate);
    *(__le16 *)&data[10] = cpu_to_le16(ctrl->wPFrameRate);
    *(__le16 *)&data[12] = cpu_to_le16(ctrl->wCompQuality);
    *(__le16 *)&data[14] = cpu_to_le16(ctrl->wCompWindowSize);
    *(__le16 *)&data[16] = cpu_to_le16(ctrl->wDelay);
    put_unaligned_le32(ctrl->dwMaxVideoFrameSize, &data[18]);      /* data[18] = dwMaxVideoFrameSize ǿ�ƶ��븳ֵ */
    put_unaligned_le32(ctrl->dwMaxPayloadTransferSize, &data[22]);

	   if (size == 34) 
	{
        put_unaligned_le32(ctrl->dwClockFrequency, &data[26]);
        data[30] = ctrl->bmFramingInfo;
        data[31] = ctrl->bPreferedVersion;
        data[32] = ctrl->bMinVersion;
        data[33] = ctrl->bMaxVersion;
    }

	/* ��usbͨ�� */
	ret = usb_control_msg(my_usb_device,pipe, SET_CUR, type,  VS_PROBE_CONTROL << 8,0 << 8 | uv_streaming_interface, data,  size, 5000);
    kfree(data);
	if(ret < 0) 
		return ret;

	return 0;
}

static int usb_v4l2_get_streaming_params(struct usb_v4l2_streaming_control *ctrl)
{
    unsigned char *data;
    unsigned short size = 0 ;
    unsigned char type = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    unsigned int pipe;
    int ret = 0;

     /* ����  �������ݴ�С   */
	size = 0x0100 > 0x0110 ? 34 : 26;

	/* ���ùܵ�pipe  */
	pipe = (GET_CUR & 0x80) ? usb_rcvctrlpipe(my_usb_device, 0) : usb_sndctrlpipe(my_usb_device, 0);

     /* ������������   */
    type |= (GET_CUR & 0x80) ? USB_DIR_IN : USB_DIR_OUT;

	data = kmalloc(size, GFP_KERNEL);
	if(data == NULL) return -ENOMEM;

    /* �򵥴���õ�dataֵ */
	ret = usb_control_msg(my_usb_device, pipe,GET_CUR, type, VS_PROBE_CONTROL << 8,0 << 8 | uv_streaming_interface, data, size, 5000);
    if(ret < 0) return ret;
	
    /* ����data���� usb_v4l2_streaming_control*/
    ctrl->bmHint = le16_to_cpup((__le16 *)&data[0]);
	ctrl->bFormatIndex = data[2];
	ctrl->bFrameIndex = data[3];
	ctrl->dwFrameInterval = le32_to_cpup((__le32 *)&data[4]);
	ctrl->wKeyFrameRate = le16_to_cpup((__le16 *)&data[8]);
	ctrl->wPFrameRate = le16_to_cpup((__le16 *)&data[10]);
	ctrl->wCompQuality = le16_to_cpup((__le16 *)&data[12]);
	ctrl->wCompWindowSize = le16_to_cpup((__le16 *)&data[14]);
	ctrl->wDelay = le16_to_cpup((__le16 *)&data[16]);
	ctrl->dwMaxVideoFrameSize = get_unaligned_le32(&data[18]);
	ctrl->dwMaxPayloadTransferSize = get_unaligned_le32(&data[22]);

	if (size == 34) 
	{
		ctrl->dwClockFrequency = get_unaligned_le32(&data[26]);
		ctrl->bmFramingInfo = data[30];
		ctrl->bPreferedVersion = data[31];
		ctrl->bMinVersion = data[32];
		ctrl->bMaxVersion = data[33];
	}
	else 
	{
		//ctrl->dwClockFrequency = video->dev->clock_frequency;
		ctrl->bmFramingInfo = 0;
		ctrl->bPreferedVersion = 0;
		ctrl->bMinVersion = 0;
		ctrl->bMaxVersion = 0;
	}

	kfree(data);

	return 0;
}

static int usb_v4l2_set_streaming_params(struct usb_v4l2_streaming_control *ctrl)
{
    unsigned char *data;
    unsigned short size = 0 ;
    unsigned char type = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    unsigned int pipe;
    int ret = 0;

    /* ����  �������ݴ�С   */
	size = 0x0100 > 0x0110 ? 34 : 26;

	/* ����Ҫ  ���������   */
    data = kzalloc(size, GFP_KERNEL);
    if (data == NULL)  return -ENOMEM;

    *(__le16 *)&data[0] = cpu_to_le16(ctrl->bmHint);
    data[2] = ctrl->bFormatIndex;
    data[3] = ctrl->bFrameIndex;
    *(__le32 *)&data[4] = cpu_to_le32(ctrl->dwFrameInterval);
    *(__le16 *)&data[8] = cpu_to_le16(ctrl->wKeyFrameRate);
    *(__le16 *)&data[10] = cpu_to_le16(ctrl->wPFrameRate);
    *(__le16 *)&data[12] = cpu_to_le16(ctrl->wCompQuality);
    *(__le16 *)&data[14] = cpu_to_le16(ctrl->wCompWindowSize);
    *(__le16 *)&data[16] = cpu_to_le16(ctrl->wDelay);
    put_unaligned_le32(ctrl->dwMaxVideoFrameSize, &data[18]);
    put_unaligned_le32(ctrl->dwMaxPayloadTransferSize, &data[22]);

    if (size == 34) {
        put_unaligned_le32(ctrl->dwClockFrequency, &data[26]);
        data[30] = ctrl->bmFramingInfo;
        data[31] = ctrl->bPreferedVersion;
        data[32] = ctrl->bMinVersion;
        data[33] = ctrl->bMaxVersion;
    }

    /* ���ùܵ�pipe  */
    pipe = (SET_CUR & 0x80) ? usb_rcvctrlpipe(my_usb_device, 0): usb_sndctrlpipe(my_usb_device, 0);

	/* ������������   */
    type |= (SET_CUR & 0x80) ? USB_DIR_IN : USB_DIR_OUT;

    /* �������� */
    ret = usb_control_msg(my_usb_device, pipe, SET_CUR, type, VS_COMMIT_CONTROL << 8,0 << 8 | uv_streaming_interface, data, size, 5000);
    if(ret < 0) return ret;

    kfree(data);
    
    return  0;
}

/*  �ж������Ƿ�ȫ���������             */
static void urb_tranfer_complete(struct urb *urb)
{
    struct usb_v4l2_buff *temp_usb_v4l2_buff;
	int var=0;
	unsigned char *src,*dest;
	unsigned int frame_len,copy_len,max_buff_len;
	
    /* ���urb״̬ */
    switch(urb->status)
    	{
            case 0:break;
		    default: { printk("Non-zero status (%d) in video completion handler.\n", urb->status);   return ;}
	    }

	if( !list_empty(&usb_v4l2_queue.ker_buf_queue))
	  { 
          temp_usb_v4l2_buff = list_first_entry(&usb_v4l2_queue.ker_buf_queue,struct usb_v4l2_buff,driver);
    		
	      /* ��һ��urb���е�urb_buffȫ������һ��usb_v4l2_buffer���� */
		  for( var = 0 ; var < urb->number_of_packets ; urb++ )
		  {
		      /* �ж�urb״̬�Ƿ���ȷ */
              if (urb->iso_frame_desc[var].status < 0) 
			   {
    			  printk("USB isochronous frame lost (%d).\n", urb->iso_frame_desc[var].status);
    		      continue;
    		   }
		   
		     /* ��urb��Ѱ�ҵ���ʼ��ַ */
		     src = urb->transfer_buffer + urb->iso_frame_desc[var].offset;
     
             /* ��usb_v4l2_buff��Ѱ�ҵ�Ŀ�ĵ�ַ */    
			 dest = usb_v4l2_queue.start_addr + temp_usb_v4l2_buff->buffer.m.offset + temp_usb_v4l2_buff->buffer.bytesused;

			 /* ֡���� */
			 frame_len = urb->iso_frame_desc[var].actual_length;

             /* ����ַ�Ƿ���ȷ */
              if (frame_len < 2 || src[0] < 2 || src[0] > frame_len)
                continue;
			  
			  if (src[1] & UVC_STREAM_ERR)
			   {
                 printk("Dropping payload (error bit set).\n");
                 continue;
               }

             /* ����src  ��buff����Ƚϵõ�������С */
			 frame_len -= src[0];                       /* һ֡ȥ��ͷ�������ݳ��� */
			 max_buff_len = temp_usb_v4l2_buff->buffer.length - temp_usb_v4l2_buff->buffer.bytesused;
			 copy_len = min(frame_len,max_buff_len);

             /* �������� */
			 memcpy(dest, src + src[0], copy_len);
			 temp_usb_v4l2_buff->buffer.bytesused += copy_len;

			 /* �ж��Ƿ񿽱���ϣ������û���״̬ */
			 if(frame_len > copy_len)
			 	temp_usb_v4l2_buff->buff_state = VIDEOBUF_DONE;

			 /*�������������EOF������Ϊ����� */
            if (src[1] & UVC_STREAM_EOF && temp_usb_v4l2_buff->buffer.bytesused != 0) 
			{
                printk("Frame complete (EOF found).\n");
                if (frame_len == 0)
                    printk("EOF in empty payload.\n");
                temp_usb_v4l2_buff->buff_state = VIDEOBUF_DONE;
            }
		  }
		  
		 /* һ֡���ݽ����꣬���ѽ��� */
         if(temp_usb_v4l2_buff->buff_state == VIDEOBUF_DONE || temp_usb_v4l2_buff->buff_state == VIDEOBUF_ERROR)
         	{
				list_del(&temp_usb_v4l2_buff->user);
			    wake_up(&temp_usb_v4l2_buff->pocs_wait);
		    }
	  }
	/* �ٴ��ύURB���ٻ������ */
	if(usb_submit_urb( urb, GFP_ATOMIC) < 0)
      printk("failed to resubmit urb!");
}


static void  uninit_urb(void)
{
     int var = 0;
    for (var = 0; var < 5; ++var)
	{
        if (usb_v4l2_queue.urb_buff[var])
        {
            usb_buffer_free(my_usb_device, usb_v4l2_queue.urb_size, usb_v4l2_queue.urb_buff[var], usb_v4l2_queue.urb_dma[var]);
            usb_v4l2_queue.urb_buff[var] = NULL;
        }

        if (usb_v4l2_queue.urb[var])
        {
            usb_free_urb(usb_v4l2_queue.urb[var]);
            usb_v4l2_queue.urb[var] = NULL;
        }
    }
}


static int alloc_init_urb(void)
{
    unsigned short pack_size;
	unsigned int   fmt_size;
    unsigned int   buffs_size;
	unsigned int   pack_num;
	int var = 0;
	struct urb *temp_urb; 
	

	/* �������size */
    pack_size = 1024;                                                /* ��ʵ����һ���ܴ�����ֽ��� */
	fmt_size = my_usb_v4l2_streaming_control.dwMaxVideoFrameSize;    /* ���õĸ�ʽ��һ֡�����ֽ�����Ӧ�ó�������� */
	pack_num = DIV_ROUND_UP(fmt_size ,pack_size);                    /* ����ó�һ��urb�� �������ٸ�pack_size */
	if(pack_num > 32) pack_num = 32;
    buffs_size = pack_size * pack_num;                               /* һ��urb���������е�pack_size��С */
	

	/* ����urb_buffer��urb */
	for(var=0;var<5;var++)
	{
      usb_v4l2_queue.urb_buff[var] = usb_buffer_alloc(my_usb_device, buffs_size, GFP_KERNEL | __GFP_NOWARN,&usb_v4l2_queue.urb_dma[var]);
      usb_v4l2_queue.urb[var] = usb_alloc_urb(pack_num, GFP_KERNEL);

     if(!usb_v4l2_queue.urb_buff[var] || !usb_v4l2_queue.urb[var])
     	{
           uninit_urb();
           return -ENOMEM;
	    }
	}

	/* ����urb */
    for(var=0;var<5;var++)
    {
        temp_urb = usb_v4l2_queue.urb[var];
		temp_urb->dev = my_usb_device;
        temp_urb->context = NULL;
	    temp_urb->pipe = usb_rcvisocpipe(my_usb_device, 0x81);
	    temp_urb->transfer_flags = 	URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
		temp_urb->interval = 1;
		temp_urb->transfer_buffer = usb_v4l2_queue.urb_buff[var];
		temp_urb->transfer_dma = usb_v4l2_queue.urb_dma[var];
		temp_urb->complete = urb_tranfer_complete;       /* urb������ɺ󣬽�urb_buff���ݿ�����usb_v4l2_queue-->usb_v4l2_buf-->v4l2_buffer���� */
		temp_urb->number_of_packets = pack_num;
		temp_urb->transfer_buffer_length = buffs_size;   /* һ֡��buff��С */

		/* Ϊÿ��urb��ÿ��urb_buff����ƫ��ֵ */
		for(var=0;var<pack_num;var++)
			{
               temp_urb->iso_frame_desc[var].offset = var*pack_size;
               temp_urb->iso_frame_desc[var].length = pack_size;
		    }
	}
    return 0;
}

static int my_v4l2_streamon(struct file *file, void *fh, enum v4l2_buf_type var)
{
   int ret = 0;
   /* ������Ƶ�������Ƿ���� */
   ret = usb_v4l2_try_streaming_params(&my_usb_v4l2_streaming_control);
   printk("usb v4l2 try streaming params return value = %d \n",ret);

   /* ��ȡ��Ƶ������   */
   ret = usb_v4l2_get_streaming_params(&my_usb_v4l2_streaming_control);
   printk("usb v4l2 get streaming params return value = %d \n",ret);

   /* ������Ƶ������ */
   ret = usb_v4l2_set_streaming_params(&my_usb_v4l2_streaming_control);
   printk("usb v4l2 set streaming params return value = %d \n",ret);
   
   /* ���ǰ��Ƶ���ӿ� */
   usb_set_interface(my_usb_device, uv_streaming_interface, 8);
   
   /* ��ʼ��URB�� */
   ret = alloc_init_urb();
   if(ret < 0) printk("alloc and init urb pack failed ! error code = %d \n",ret );
   
   /* ����URB�� */
   for(var=0; var<5; var++)
   	{
   	   /* �� urb->cpmplete �����в��Ϸ���urb���Խ���֡���� */
      if((ret =  usb_submit_urb(usb_v4l2_queue.urb[var], GFP_KERNEL)))
      	{
          printk(" usb submit error ! error code = %d \n",ret);
		  uninit_urb();
		  return ret;
	    }
   	}
   
   return 0;
}

static int my_v4l2_streamoff(struct file *file, void *fh, enum v4l2_buf_type i)
{
   	struct urb *temp_urb;
    int var;

    /* kill URB */
	for (var = 0; var < 5; ++var) 
	{
		if ((temp_urb = usb_v4l2_queue.urb[var]) == NULL)
			continue;
		usb_kill_urb(temp_urb);
	}

    /* �ͷ� URB */
    uninit_urb();

    /* ����VideoStreaming InterfaceΪsetting 0 */
    usb_set_interface(my_usb_device, uv_streaming_interface, 0);
  
    return 0;
}

static struct   v4l2_ioctl_ops  my_v4l2_ioctl_ops =
{
   .vidioc_querycap = my_v4l2_query_cap,
   .vidioc_enum_fmt_vid_cap = my_v4l2_enum_fmt_vid_cap,
   .vidioc_g_fmt_vid_cap  = my_v4l2_g_fmt_vid_cap,
   .vidioc_try_fmt_vid_cap = my_v4l2_try_fmt_vid_cap,
   .vidioc_s_fmt_vid_cap = my_v4l2_s_fmt_vid_cap,

   .vidioc_reqbufs = my_v4l2_reqbufs,
   .vidioc_querybuf = my_v4l2_querybuf,
   .vidioc_qbuf = my_v4l2_qbuf,
   .vidioc_dqbuf = my_v4l2_dqbuf,
   
   .vidioc_streamon = my_v4l2_streamon,
   .vidioc_streamoff = my_v4l2_streamoff,
   
};


/*************************************v4l2_file_operations***************************************/
static int my_v4l2_open(struct file *file )
{
   return 0;
}
  

static int my_v4l2_close(struct file *file)
{
    
	return 0;
}


static void pri_vma_open(struct vm_area_struct *area)
{
   struct usb_v4l2_buff *pri_buffer;
   pri_buffer = area->vm_private_data ;
   pri_buffer->mmap_state++;
}


static void pri_vma_close(struct vm_area_struct * area)
{
	
	struct usb_v4l2_buff *pri_buffer;
	pri_buffer = area->vm_private_data ;
	pri_buffer->mmap_state--;
}

static struct vm_operations_struct vma_ops =
{
    .open = pri_vma_open,
    .close = pri_vma_close,
};

static int my_v4l2_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct usb_v4l2_buff *buff;
    unsigned long ker_addr;
    unsigned long usr_start,usr_size;
	struct page *ker_page;
	int ret = 0,i=0;

    /* ��Ӧ�ó����֪����Ļ����С */
    usr_start = vma->vm_start;
	usr_size = vma->vm_end - vma->vm_start;

    /* ����vma->vm_pgoffƫ��ֵ���ں˵�ַѰ����Ӧ��ҳ���� */
	for(i = 0 ; i < usb_v4l2_queue.count ; i++)
		{
            buff = &usb_v4l2_queue.buffers[i];
			if( buff->buffer.m.offset >> PAGE_SHIFT == vma->vm_pgoff)
    			break;
	    }

	/* ����Ƿ����� */
	if(i == usb_v4l2_queue.count || usr_size != usb_v4l2_queue.buf_size)
		{
           return -EINVAL;
	    }

	/* ����vma�ı�־Ϊ       memory mmaped I/O  */
	vma->vm_flags |= VM_IO;

	/* ȡ�û��������ַ */
	ker_addr = (unsigned long )usb_v4l2_queue.start_addr + buff->buffer.m.offset;

    while(usr_size > 0)
		{
		    /* ��ҳ��С��ÿ���������ҳ�ṹ�� */
			ker_page = vmalloc_to_page((void *)ker_addr);  /* ���ں������ַת��Ϊpageָ�� */

		    /* ��ÿһҳӳ�䵽�û��ռ� */
			ret = vm_insert_page(vma,usr_start,ker_page);
		    if(ret < 0)  return ret;

			usr_start += PAGE_SIZE;
			ker_addr  += PAGE_SIZE;
			usr_size  -= PAGE_SIZE;
    	}

	/* �ı仺����ӳ��״̬ */
	vma->vm_ops = &vma_ops;
	vma->vm_private_data = buff;
	pri_vma_open(vma);

	return 0;
}

static unsigned int my_v4l2_poll(struct file *file, struct poll_table_struct *wait )
{
    struct usb_v4l2_buff  *pri_buffer;
	unsigned int mask=0;

	/* �����Ƿ�ȫ��Ϊ�� */
	if(list_empty(&usb_v4l2_queue.usr_buf_queue ))
		return (mask | POLLERR);

    /* ���û��������ȡ��buffer */
	pri_buffer = list_first_entry(&usb_v4l2_queue.usr_buf_queue,struct usb_v4l2_buff, user);

    /* poll_wait���������̹���������� */
    poll_wait( file, &pri_buffer->pocs_wait , wait);

    /* ������ʱ���ѽ��� */
	if(pri_buffer->buff_state == VIDEOBUF_DONE || pri_buffer->buff_state == VIDEOBUF_ERROR)
		return ( mask |= POLLIN | POLLRDNORM );
   	
    return 0;
}


static struct v4l2_file_operations my_v4l2_fops =
{
   .owner = THIS_MODULE,
   .open = my_v4l2_open,             
   .release = my_v4l2_close,
   .mmap = my_v4l2_mmap,
   .poll = my_v4l2_poll,
   .ioctl = video_ioctl2,
};

static void my_v4l2_release(struct video_device *vdev)
{
}


static int my_usb_probe(struct usb_interface *intf,const struct usb_device_id *id)
{
  /* �����ǿ�������������Ƶ�������� */
  static int interface_num = 0; 
  interface_num++;

  /* ��ȡusb_device���������� */
  my_usb_device = interface_to_usbdev(intf);

  /* ����ӿ���������� */
  if(interface_num == 1) uv_ctrl_interface = intf->cur_altsetting->desc.bInterfaceNumber;     
  if(interface_num == 2) uv_streaming_interface = intf->cur_altsetting->desc.bInterfaceNumber;

  if(interface_num == 2)
  	{
	  /* ����video_device */
      my_video_device = video_device_alloc();
	  
	  /* ����video_device */
      my_video_device->fops       = &my_v4l2_fops;
      my_video_device->ioctl_ops  = &my_v4l2_ioctl_ops;
      my_video_device->release    = my_v4l2_release;
	  
	  /* ע��video_device */
      video_register_device(my_video_device,VFL_TYPE_GRABBER,-1);
  }	  
  return 0;
}

static void my_usb_disconnect(struct usb_interface *intf)
{
  static int interface_num = 0; 
  interface_num++;
  
  if(interface_num == 2)
  	{
      video_unregister_device(my_video_device);
	  video_device_release(my_video_device);
    }

}

static const struct usb_device_id my_usb_id_table[] =
{
    {USB_INTERFACE_INFO(USB_CLASS_VIDEO, 1,0)},
  	{USB_INTERFACE_INFO(USB_CLASS_VIDEO, 2,0)}
};

/* ����һ��usb_driver     */
static struct usb_driver my_usb_driver =
{
   .name  = "my_usb_v4l2",
   .probe = my_usb_probe,
   .id_table = my_usb_id_table,
   .disconnect = my_usb_disconnect,
};

static int usb_driver_init(void)
{
   usb_register(&my_usb_driver);
   return 0;
}

static void usb_driver_exit(void)
{
  usb_deregister(&my_usb_driver);
}

module_init(usb_driver_init);
module_exit(usb_driver_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zsy  1225405552@qq.com");



