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

/* 定义一个usb_device 结构体 */
static struct usb_device *my_usb_device;

/* 控制描述符编号 */
static int uv_ctrl_interface;

/* 视频流描述符编号 */
static int uv_streaming_interface;

/* 定义一个vodeo_device结构体 */
static struct video_device *my_video_device; 

/* 定义一个v4l2_formt  结构体 */
static struct v4l2_format my_v4l2_format;

/* 本地缓存 */
static struct usb_v4l2_buff 
{
   struct v4l2_buffer buffer;
   int buff_state;
   int mmap_state;
   wait_queue_head_t pocs_wait;  /* 查询数据，进程休眠队列 */

   struct list_head driver;
   struct list_head user;   
}usb_v4l2_buff;

/* 本地队列 */
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

/* 视频流数据 */
struct usb_v4l2_streaming_control {
	__u16 bmHint;
	__u8  bFormatIndex;   /* 视频格式索引     */
	__u8  bFrameIndex;    /* 视频帧索引 */
	__u32 dwFrameInterval; /* 视频帧间隔 */
	__u16 wKeyFrameRate;
	__u16 wPFrameRate;
	__u16 wCompQuality;
	__u16 wCompWindowSize;
	__u16 wDelay;
	__u32 dwMaxVideoFrameSize;       /* 最大视频帧大小 */
	__u32 dwMaxPayloadTransferSize;  /*  */
	__u32 dwClockFrequency;      /* 时钟频率 */
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
	
  	/* 设置v4l2_capability结构体      */
    strcpy(cap->driver, "usb_v4l2_camera");
    strcpy(cap->card,"usb_v4l2_camera");
    cap->version = 1;
	cap->capabilities = V4L2_CAP_VBI_CAPTURE | V4L2_CAP_STREAMING ;
	return 0;
}

static int my_v4l2_enum_fmt_vid_cap(struct file *file, void *fh,struct v4l2_fmtdesc *fmtdesc)
{
   /*判断支持格式*/
   if(fmtdesc->index >= 1) return -EINVAL;
   
   /* 设置v4l2_fmtdesc结构体 */
   strcpy(fmtdesc->description,"4:2:2, packed, YUYV");
          fmtdesc->pixelformat = V4L2_PIX_FMT_YUYV;
   
   return 0;
}

static int my_v4l2_g_fmt_vid_cap(struct file *file, void *fh,struct v4l2_format *fmt)
{
   /* 应用程序得到格式 */
   memcpy(fmt,&my_v4l2_format,sizeof(my_v4l2_format)); 
   return 0;
}

static int my_v4l2_try_fmt_vid_cap(struct file *file, void *fh,struct v4l2_format *fmt)
{
    /* 判断格式，类型是否正确 */
    if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)     return -EINVAL;
    if (fmt->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)return -EINVAL;

    /* 设置一帧，宽度，高度 */
    fmt->fmt.pix.width = 352;                                                    /* 帧字节宽度 */
	fmt->fmt.pix.height = 288;    											     /* 帧字节高度 */
	fmt->fmt.pix.bytesperline = fmt->fmt.pix.width * 16 ;                        /* 帧比特宽度 */
	fmt->fmt.pix.sizeimage = fmt->fmt.pix.bytesperline * fmt->fmt.pix.height ;   /* 帧比特大小 */

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
   /* 从应用程序得知需要缓冲区的大小 */
   int frame_num = reqbuf->count;
   /* 一帧大小 */
   int frame_size = PAGE_ALIGN(my_v4l2_format.fmt.pix.sizeimage);
   int var=0;
   /* 缓存起始地址 */
   void *mem = NULL;

   /* 必要的检测 */
   if(free_usb_v4l2_queue() < 0) return -1;
   if(frame_num == 0) return -1;

   

   /* 为所有帧分配32位连续的用户空间缓存 */
   for( ; frame_num>0 ; frame_num--)
   	{
      mem = vmalloc_32(var * frame_size);
	  if(mem != NULL)  break;
   }

   if(mem == NULL)  return ENOMEM;

   /*初始化缓存队列*/
   memset(&usb_v4l2_queue, 0, sizeof(usb_v4l2_queue));   
   INIT_LIST_HEAD(&usb_v4l2_queue.usr_buf_queue);
   INIT_LIST_HEAD(&usb_v4l2_queue.ker_buf_queue);

   /* 设置申请的每一个缓存 */
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

   /* 设置整个缓存 */
   usb_v4l2_queue.start_addr = mem;
   usb_v4l2_queue.count = frame_num;
   usb_v4l2_queue.buf_size = frame_size;

   return frame_num;
}

static int my_v4l2_querybuf(struct file *file, void *fh, struct v4l2_buffer *v4l2_buf)
{
    /* 判断应用程序参数是否正确 */
    if(v4l2_buf->index > usb_v4l2_queue.count ) return -EINVAL;

    /* 从本地队列根据应用程序取出对应的缓存 */
	memcpy(v4l2_buf,&usb_v4l2_queue.buffers[v4l2_buf->index].buffer ,sizeof(*v4l2_buf));

	/* 根据usb_v4l2_queue.usb_v4l2_buffers状态设置v4l2_buf标志位 */
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

   /* 检测传参 */
   if(v4l2_buff->index > usb_v4l2_queue.count || v4l2_buff->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || v4l2_buff->memory != V4L2_MEMORY_MMAP )
      return -EINVAL;
   
   /* 修改缓存状态 */
   temp_usb_v4l2_buff = &usb_v4l2_queue.buffers[v4l2_buff->index]; 
   if(temp_usb_v4l2_buff->buff_state != VIDEOBUF_IDLE)  return -EINVAL;
   
   /* 加入队列 */
   list_add(&temp_usb_v4l2_buff->user, &usb_v4l2_queue.usr_buf_queue);
   list_add(&temp_usb_v4l2_buff->driver, &usb_v4l2_queue.ker_buf_queue);

   return 0;
}

static int my_v4l2_dqbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
    struct usb_v4l2_buff *temp_usb_v4l2_buff;

    /* 判断队列是否为空 */
    if(list_empty(&usb_v4l2_queue.usr_buf_queue)) return -EINVAL;

    /* 从队列取出缓冲区 */
    temp_usb_v4l2_buff = list_first_entry(&usb_v4l2_queue.usr_buf_queue,struct usb_v4l2_buff,user);

	/* 修改缓冲区状态 */
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

    /* 把usb_v4l2_streaming_control成员全部清零 ,做一些初始化*/
    memset(ctrl, 0 , sizeof(*ctrl));
    ctrl->bmHint = 1;
    ctrl->bFormatIndex = 1;
    ctrl->bFrameIndex = 2;
    ctrl->dwFrameInterval = 333333;   /* 帧间隔时间==>31fps */
   
    /* 设置  传输数据大小   */
    size = 0x0100 > 0x0110 ? 34 : 26;

    /* 设置管道pipe  */
    pipe = (SET_CUR & 0x80) ? usb_rcvctrlpipe(my_usb_device, 0) : usb_sndctrlpipe(my_usb_device, 0);

     /* 设置请求类型   */
    type |= (SET_CUR & 0x80) ? USB_DIR_IN : USB_DIR_OUT;

    /* 根据	usb_v4l2_streaming_control 设置要传输的值  */
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
    put_unaligned_le32(ctrl->dwMaxVideoFrameSize, &data[18]);      /* data[18] = dwMaxVideoFrameSize 强制对齐赋值 */
    put_unaligned_le32(ctrl->dwMaxPayloadTransferSize, &data[22]);

	   if (size == 34) 
	{
        put_unaligned_le32(ctrl->dwClockFrequency, &data[26]);
        data[30] = ctrl->bmFramingInfo;
        data[31] = ctrl->bPreferedVersion;
        data[32] = ctrl->bMinVersion;
        data[33] = ctrl->bMaxVersion;
    }

	/* 简单usb通信 */
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

     /* 设置  传输数据大小   */
	size = 0x0100 > 0x0110 ? 34 : 26;

	/* 设置管道pipe  */
	pipe = (GET_CUR & 0x80) ? usb_rcvctrlpipe(my_usb_device, 0) : usb_sndctrlpipe(my_usb_device, 0);

     /* 设置请求类型   */
    type |= (GET_CUR & 0x80) ? USB_DIR_IN : USB_DIR_OUT;

	data = kmalloc(size, GFP_KERNEL);
	if(data == NULL) return -ENOMEM;

    /* 简单传输得到data值 */
	ret = usb_control_msg(my_usb_device, pipe,GET_CUR, type, VS_PROBE_CONTROL << 8,0 << 8 | uv_streaming_interface, data, size, 5000);
    if(ret < 0) return ret;
	
    /* 根据data设置 usb_v4l2_streaming_control*/
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

    /* 设置  传输数据大小   */
	size = 0x0100 > 0x0110 ? 34 : 26;

	/* 设置要  传输的数据   */
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

    /* 设置管道pipe  */
    pipe = (SET_CUR & 0x80) ? usb_rcvctrlpipe(my_usb_device, 0): usb_sndctrlpipe(my_usb_device, 0);

	/* 设置请求类型   */
    type |= (SET_CUR & 0x80) ? USB_DIR_IN : USB_DIR_OUT;

    /* 启动传输 */
    ret = usb_control_msg(my_usb_device, pipe, SET_CUR, type, VS_COMMIT_CONTROL << 8,0 << 8 | uv_streaming_interface, data, size, 5000);
    if(ret < 0) return ret;

    kfree(data);
    
    return  0;
}

/*  判断数据是否全部传输完毕             */
static void urb_tranfer_complete(struct urb *urb)
{
    struct usb_v4l2_buff *temp_usb_v4l2_buff;
	int var=0;
	unsigned char *src,*dest;
	unsigned int frame_len,copy_len,max_buff_len;
	
    /* 检查urb状态 */
    switch(urb->status)
    	{
            case 0:break;
		    default: { printk("Non-zero status (%d) in video completion handler.\n", urb->status);   return ;}
	    }

	if( !list_empty(&usb_v4l2_queue.ker_buf_queue))
	  { 
          temp_usb_v4l2_buff = list_first_entry(&usb_v4l2_queue.ker_buf_queue,struct usb_v4l2_buff,driver);
    		
	      /* 将一个urb所有的urb_buff全部存入一个usb_v4l2_buffer当中 */
		  for( var = 0 ; var < urb->number_of_packets ; urb++ )
		  {
		      /* 判断urb状态是否正确 */
              if (urb->iso_frame_desc[var].status < 0) 
			   {
    			  printk("USB isochronous frame lost (%d).\n", urb->iso_frame_desc[var].status);
    		      continue;
    		   }
		   
		     /* 从urb中寻找到起始地址 */
		     src = urb->transfer_buffer + urb->iso_frame_desc[var].offset;
     
             /* 从usb_v4l2_buff中寻找到目的地址 */    
			 dest = usb_v4l2_queue.start_addr + temp_usb_v4l2_buff->buffer.m.offset + temp_usb_v4l2_buff->buffer.bytesused;

			 /* 帧长度 */
			 frame_len = urb->iso_frame_desc[var].actual_length;

             /* 检测地址是否正确 */
              if (frame_len < 2 || src[0] < 2 || src[0] > frame_len)
                continue;
			  
			  if (src[1] & UVC_STREAM_ERR)
			   {
                 printk("Dropping payload (error bit set).\n");
                 continue;
               }

             /* 根据src  和buff计算比较得到拷贝大小 */
			 frame_len -= src[0];                       /* 一帧去掉头部的数据长度 */
			 max_buff_len = temp_usb_v4l2_buff->buffer.length - temp_usb_v4l2_buff->buffer.bytesused;
			 copy_len = min(frame_len,max_buff_len);

             /* 拷贝数据 */
			 memcpy(dest, src + src[0], copy_len);
			 temp_usb_v4l2_buff->buffer.bytesused += copy_len;

			 /* 判断是否拷贝完毕，再设置缓存状态 */
			 if(frame_len > copy_len)
			 	temp_usb_v4l2_buff->buff_state = VIDEOBUF_DONE;

			 /*如果缓存设置了EOF，则标记为已完成 */
            if (src[1] & UVC_STREAM_EOF && temp_usb_v4l2_buff->buffer.bytesused != 0) 
			{
                printk("Frame complete (EOF found).\n");
                if (frame_len == 0)
                    printk("EOF in empty payload.\n");
                temp_usb_v4l2_buff->buff_state = VIDEOBUF_DONE;
            }
		  }
		  
		 /* 一帧数据接收完，唤醒进程 */
         if(temp_usb_v4l2_buff->buff_state == VIDEOBUF_DONE || temp_usb_v4l2_buff->buff_state == VIDEOBUF_ERROR)
         	{
				list_del(&temp_usb_v4l2_buff->user);
			    wake_up(&temp_usb_v4l2_buff->pocs_wait);
		    }
	  }
	/* 再次提交URB，再获得数据 */
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
	

	/* 计算各种size */
    pack_size = 1024;                                                /* 事实传输一次能传输的字节数 */
	fmt_size = my_usb_v4l2_streaming_control.dwMaxVideoFrameSize;    /* 设置的格式中一帧数据字节数（应用程序决定） */
	pack_num = DIV_ROUND_UP(fmt_size ,pack_size);                    /* 计算得出一个urb包 包含多少个pack_size */
	if(pack_num > 32) pack_num = 32;
    buffs_size = pack_size * pack_num;                               /* 一个urb包含的所有的pack_size大小 */
	

	/* 分配urb_buffer和urb */
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

	/* 设置urb */
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
		temp_urb->complete = urb_tranfer_complete;       /* urb传输完成后，将urb_buff数据拷贝到usb_v4l2_queue-->usb_v4l2_buf-->v4l2_buffer里面 */
		temp_urb->number_of_packets = pack_num;
		temp_urb->transfer_buffer_length = buffs_size;   /* 一帧的buff大小 */

		/* 为每个urb的每个urb_buff设置偏移值 */
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
   /* 测试视频流参数是否可用 */
   ret = usb_v4l2_try_streaming_params(&my_usb_v4l2_streaming_control);
   printk("usb v4l2 try streaming params return value = %d \n",ret);

   /* 获取视频流参数   */
   ret = usb_v4l2_get_streaming_params(&my_usb_v4l2_streaming_control);
   printk("usb v4l2 get streaming params return value = %d \n",ret);

   /* 设置视频流参数 */
   ret = usb_v4l2_set_streaming_params(&my_usb_v4l2_streaming_control);
   printk("usb v4l2 set streaming params return value = %d \n",ret);
   
   /* 激活当前视频流接口 */
   usb_set_interface(my_usb_device, uv_streaming_interface, 8);
   
   /* 初始化URB包 */
   ret = alloc_init_urb();
   if(ret < 0) printk("alloc and init urb pack failed ! error code = %d \n",ret );
   
   /* 发送URB包 */
   for(var=0; var<5; var++)
   	{
   	   /* 在 urb->cpmplete 函数中不断发送urb包以接收帧数据 */
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

    /* 释放 URB */
    uninit_urb();

    /* 设置VideoStreaming Interface为setting 0 */
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

    /* 从应用程序得知所需的缓存大小 */
    usr_start = vma->vm_start;
	usr_size = vma->vm_end - vma->vm_start;

    /* 根据vma->vm_pgoff偏移值在内核地址寻找相应的页缓存 */
	for(i = 0 ; i < usb_v4l2_queue.count ; i++)
		{
            buff = &usb_v4l2_queue.buffers[i];
			if( buff->buffer.m.offset >> PAGE_SHIFT == vma->vm_pgoff)
    			break;
	    }

	/* 检测是否有误 */
	if(i == usb_v4l2_queue.count || usr_size != usb_v4l2_queue.buf_size)
		{
           return -EINVAL;
	    }

	/* 设置vma的标志为       memory mmaped I/O  */
	vma->vm_flags |= VM_IO;

	/* 取得缓存虚拟地址 */
	ker_addr = (unsigned long )usb_v4l2_queue.start_addr + buff->buffer.m.offset;

    while(usr_size > 0)
		{
		    /* 按页大小，每个缓存分配页结构体 */
			ker_page = vmalloc_to_page((void *)ker_addr);  /* 将内核虚拟地址转化为page指针 */

		    /* 将每一页映射到用户空间 */
			ret = vm_insert_page(vma,usr_start,ker_page);
		    if(ret < 0)  return ret;

			usr_start += PAGE_SIZE;
			ker_addr  += PAGE_SIZE;
			usr_size  -= PAGE_SIZE;
    	}

	/* 改变缓存区映射状态 */
	vma->vm_ops = &vma_ops;
	vma->vm_private_data = buff;
	pri_vma_open(vma);

	return 0;
}

static unsigned int my_v4l2_poll(struct file *file, struct poll_table_struct *wait )
{
    struct usb_v4l2_buff  *pri_buffer;
	unsigned int mask=0;

	/* 缓存是否全部为空 */
	if(list_empty(&usb_v4l2_queue.usr_buf_queue ))
		return (mask | POLLERR);

    /* 从用户缓存队列取出buffer */
	pri_buffer = list_first_entry(&usb_v4l2_queue.usr_buf_queue,struct usb_v4l2_buff, user);

    /* poll_wait，将本进程挂入队列休眠 */
    poll_wait( file, &pri_buffer->pocs_wait , wait);

    /* 有数据时唤醒进程 */
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
  /* 区分是控制描述符，视频流描述符 */
  static int interface_num = 0; 
  interface_num++;

  /* 提取usb_device，供传输用 */
  my_usb_device = interface_to_usbdev(intf);

  /* 保存接口描述符编号 */
  if(interface_num == 1) uv_ctrl_interface = intf->cur_altsetting->desc.bInterfaceNumber;     
  if(interface_num == 2) uv_streaming_interface = intf->cur_altsetting->desc.bInterfaceNumber;

  if(interface_num == 2)
  	{
	  /* 申请video_device */
      my_video_device = video_device_alloc();
	  
	  /* 设置video_device */
      my_video_device->fops       = &my_v4l2_fops;
      my_video_device->ioctl_ops  = &my_v4l2_ioctl_ops;
      my_video_device->release    = my_v4l2_release;
	  
	  /* 注册video_device */
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

/* 定义一个usb_driver     */
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



