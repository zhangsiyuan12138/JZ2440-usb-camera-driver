#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/random.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/videodev2.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/highmem.h>
#include <linux/freezer.h>
#include <media/videobuf-vmalloc.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>


/* ����һ��video_device   */
static struct video_device *my_video_device;

/* ����ͷ���ݸ�ʽ�ṹ�� */
static struct v4l2_format my_v4l2_format;

/* ������� */
static struct videobuf_queue my_videobuf_queue;

/* ���������� */
static  spinlock_t my_queue_slock;

/* �����ϱ���ʱ��*/
static struct timer_list my_timer;

/* ���ؽ��̶��У���������*/
static struct list_head my_local_queue;

/*��������ͷ���ݹ��캯��*/
#include"my_fill_buf.c"

/**************************************************videobuf_queue_ops********************************************************/

static  int my_buf_setup(struct videobuf_queue *q,unsigned int *count, unsigned int *size)
{
   /*���¼���buff size����������Ҫ�˷ѿռ�*/
   *size = my_v4l2_format.fmt.pix.sizeimage;

   if(*count == 0) *count=32;
   return 0;
}


static	int my_buf_prepare(struct videobuf_queue *q, struct videobuf_buffer *vb, enum v4l2_field field)
{
   /*����video_buf�Ĵ�С�����*/
   vb->size = my_v4l2_format.fmt.pix.sizeimage;
   vb->bytesperline = my_v4l2_format.fmt.pix.bytesperline;
   vb->width = my_v4l2_format.fmt.pix.width;
   vb->height = my_v4l2_format.fmt.pix.height;
   vb->field = field;

   /*����ͷ���ݳ�ʼ��*/
   my_precalculate_bars(0);

   /*����buf״̬*/
   vb->state = VIDEOBUF_PREPARED;
   return 0;
}

static	void my_buf_queue(struct videobuf_queue *q, struct videobuf_buffer *vb)
{
   /* ��buf���뱾�ض��� */
   list_add_tail(&vb->queue, &my_local_queue);
   
   /* ����buf״̬ */
   vb->state = VIDEOBUF_QUEUED;
}

static 	void my_buf_release(struct videobuf_queue *q,struct videobuf_buffer *vb)
{
   /*�ͷ�buf*/
   videobuf_vmalloc_free(vb);
  
   /*����buf״̬*/
   vb->state = VIDEOBUF_NEEDS_INIT;
}


/*
 *    
 ops->buf_setup   - calculates the size of the video buffers and avoid they to waste more than some maximum limit of RAM;
 ops->buf_prepare - fills the video buffer structs and calls videobuf_iolock() to alloc and prepare mmaped memory;
 ops->buf_queue   - advices the driver that another buffer were requested (by read() or by QBUF);
 ops->buf_release - frees any buffer that were allocated.
 
 *
 */

static struct videobuf_queue_ops my_videobuf_queue_ops =
{
   .buf_setup = my_buf_setup,
   .buf_prepare = my_buf_prepare,
   .buf_queue = my_buf_queue,
   .buf_release = my_buf_release,

};

/**************************************************videobuf_queue_ops********************************************************/




/***************************************************fops  start************************************************************/
static	int my_vivi_open(struct file *file)
{
  
	/*��ʼ������*/
    videobuf_queue_vmalloc_init(&my_videobuf_queue, &my_videobuf_queue_ops, NULL, &my_queue_slock, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_FIELD_INTERLACED, sizeof(struct videobuf_buffer), NULL);

	/*���ó���ʱ��*/
    my_timer.expires = jiffies + 1;

	/*����ʱ�������ں�����*/
    add_timer(&my_timer);
    return 0;
}

static	unsigned int my_vivi_poll(struct file *file, struct poll_table_struct *wait)
{
    /*�����ں˵�poll���������߽���*/
   return videobuf_poll_stream(file,&my_videobuf_queue ,  wait);
}

static	int my_vivi_mmap(struct file *file, struct vm_area_struct *vma)
{
    /*ӳ�仺����*/
   return videobuf_mmap_mapper(&my_videobuf_queue ,  vma);
}



static	int my_vivi_release(struct file *file)
{
   /*�ں�����ɾ����ʱ��*/
   del_timer(&my_timer);

   /*ֹͣ����*/
   videobuf_stop(&my_videobuf_queue);

   /*ȡ��������ӳ��*/
   videobuf_mmap_free(&my_videobuf_queue );
   return 0;
}
/***************************************************fops  end**************************************************************/




/**************************************************ioctl  start************************************************************/

static int my_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
   /* ��������  */
   strcpy(cap->driver,"zsy_vivi");
   strcpy(cap->card,"zsy_vivi");
   
   /* ����ͷ�豸�� */
   cap->version = 0x0011;
   
   /* ��������  */
   cap->capabilities = 	V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
   return 0;
}


static int my_enum_fmt_vid_cap(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
   /* ֻ֧��һ�ָ�ʽ */
   if(f->index >= 1) return -EINVAL;

   /* ���õ�ǰ��ʽΪyuyn */
   strcpy(f->description,"4:2:2,packed,YUYN");
   f->pixelformat = V4L2_PIX_FMT_YUYV;
   return 0;
}


static int my_g_fmt_vid_cap(struct file *file, void *fh, struct v4l2_format *f)
{
   /* ���ص�ǰ�ĸ�ʽ */
   memcpy(f,&my_v4l2_format,sizeof(my_v4l2_format));
   return 0;
}


static int my_try_fmt_vid_cap(struct file *file, void *fh, struct v4l2_format *f)
{
   unsigned int maxw,maxh;
   enum v4l2_field field;

   /* �����Ƿ�֧�������ʽ */
   if(f->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) return -EINVAL;
   
   field = f->fmt.pix.field;

   if (field == V4L2_FIELD_ANY)  field = V4L2_FIELD_INTERLACED;
	else if (V4L2_FIELD_INTERLACED != field)	return -EINVAL;

   /* ��������Ⱥ͸߶�  */
	maxw  = 1024;
	maxh  = 768;

    v4l_bound_align_image(&f->fmt.pix.width, 48, maxw, 2, &f->fmt.pix.height, 32, maxh, 0, 0);
	/* ������� */
	f->fmt.pix.bytesperline =(f->fmt.pix.width * 16) >> 3;
    /* ����ͼƬ��С */
	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;

	return 0;
}


static int my_s_fmt_vid_cap(struct file *file, void *fh,struct v4l2_format *f)
{
    int ret = my_try_fmt_vid_cap(file,NULL,f);
    if(ret < 0) return ret;

	 /* �����ϸ��������Ժ��˵ĸ�ʽ */
    memcpy(&my_v4l2_format,f,sizeof(my_v4l2_format));

	return ret;
}

static  int my_reqbufs(struct file *file, void *fh, struct v4l2_requestbuffers *b)
{
    /* ����һ��video���� */
    return (videobuf_reqbufs(&my_videobuf_queue, b));
}


static	int my_querybuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
   /* ��ѯ���� */
   return (videobuf_querybuf(&my_videobuf_queue,b));
}

static	int my_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
  /*�ѻ���ҵ�����*/
  return (videobuf_qbuf(&my_videobuf_queue,b));
}

static	int my_dqbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
  /* �ѻ����ó����� */
  return (videobuf_dqbuf(&my_videobuf_queue,  b, file->f_flags & O_NONBLOCK));
}

static int my_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
    /* ������ͷ */
	return videobuf_streamon(&my_videobuf_queue);
}

static int my_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
   /* �ر�����ͷ*/
	videobuf_streamoff(&my_videobuf_queue);
    return 0;
}
/*************************************************ioctl  end*************************************************************/

/* ����һ��v4l2_file_operations */
static struct v4l2_file_operations my_fops =
{
   .owner = THIS_MODULE,
   .open  = my_vivi_open,
   .release = my_vivi_release,
   .mmap  = my_vivi_mmap,
   .poll  = my_vivi_poll,
   .ioctl = video_ioctl2,

};

/* ����һ��v4l2_ioctl_ops */
static struct v4l2_ioctl_ops my_ioctl_ops = 
{
   /* ����ͷ���� */
   .vidioc_querycap = my_querycap,

   /* ���ݸ�ʽ */
   .vidioc_enum_fmt_vid_cap = my_enum_fmt_vid_cap,
   .vidioc_g_fmt_vid_cap = my_g_fmt_vid_cap ,
   .vidioc_try_fmt_vid_cap = my_try_fmt_vid_cap,
   .vidioc_s_fmt_vid_cap = my_s_fmt_vid_cap,

   /* ������ */
   .vidioc_reqbufs = my_reqbufs,
   .vidioc_querybuf = my_querybuf,
   .vidioc_qbuf = my_qbuf,
   .vidioc_dqbuf = my_dqbuf,

   /* ����/ֹͣ����ͷ */
   .vidioc_streamon = my_streamon,
   .vidioc_streamoff = my_streamoff,

};


/*��ʱ����ʱ�������������*/
void my_timer_fun(unsigned long data )
{
     struct videobuf_buffer *vb;
	 void *vbuf;
	 struct timeval ts;

    /* 1.�Ӷ�������ȡ����һ��video_buf*/
    if(list_empty(&my_local_queue))  
    	{
	     mod_timer(&my_timer, jiffies + HZ/30);
         return ;
    	}
	vb = list_entry(my_local_queue.next, struct videobuf_buffer, queue);
	if(!waitqueue_active(&vb->done)) 
	   {
	     mod_timer(&my_timer, jiffies + HZ/30);
         return ;
    	}
	
    /* 2.��������*/
	vbuf = videobuf_to_vmalloc(vb);
	my_fill_buff(vb);
	
    vb->field_count++;
	do_gettimeofday(&ts);
	vb->ts = ts;
	vb->state = VIDEOBUF_DONE;

    /* 3.����������ݵ�video_buf�Ӷ�������ɾ��*/
	list_del(&vb->queue);
	
    /* 4.���ѽ��̣���ʾbufͼ��*/
	wake_up(&vb->done);
	
    /* 5.�޸ĳ�ʱʱ�䣬30fps */
    mod_timer(&my_timer, jiffies +  HZ/30);  //new timeout in jiffies
	
}

/* �ͷź��� */
void my_video_device_release(struct video_device *vdev)
{}

int my_vivi_init(void)
{
   /* 1.����һ��video_device */
   my_video_device = video_device_alloc();

   /* 2.����video_device */
   my_video_device->release = my_video_device_release;
   my_video_device->fops = &my_fops;
   my_video_device->ioctl_ops = &my_ioctl_ops;

   /* 3.ע��video_device*/
   video_register_device(my_video_device,VFL_TYPE_GRABBER, -1);

   /* ��ʼ����ʱ��*/
   init_timer(&my_timer);
   my_timer.function = my_timer_fun;

   /* ��ʼ�������� */
   spin_lock_init(&my_queue_slock);

   /*��ʼ�����ض���*/
   INIT_LIST_HEAD(&my_local_queue);
 
   return 0;
}

void my_vivi_exit(void)
{
   video_unregister_device(my_video_device);
   video_device_release(my_video_device);
}

module_init(my_vivi_init);
module_exit(my_vivi_exit);

MODULE_LICENSE("GPL");


