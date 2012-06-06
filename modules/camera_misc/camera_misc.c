/*
 * drivers/misc/camera_misc/camera_misc.c
 *
 * Copyright (C) 2010 Motorola Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define GPIO_SENSOR_NAME "camera_misc"
#define DEBUG 2

#define TAG "camera"

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/gpio_mapping.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/videobuf-core.h>
#include <asm/io.h>
#include <plat/iommu.h>
#include <plat/iommu2.h>
#include <plat/clock.h>
#include <plat/dma.h>
#include <plat/vrfb.h>
#include <plat/display.h>

#include "camera_misc.h"
#include <../drivers/media/video/isp/isp.h>
#include <../drivers/media/video/isp/ispreg.h>
#include <../drivers/media/video/isp/ispccdc.h>
#include <../drivers/media/video/isp/isppreview.h>
#include <../drivers/media/video/omap-vout/omapvout.h>
//#include <../drivers/media/video/omap-vout/omapvout-dss.h>
#include <../drivers/media/video/omap34xxcam.h>

#include "omapvout-mem.h"
#include "omapvout-vbq.h"
#include "omapvout-dss.h"
#include "camise.h"

#include "../symsearch/symsearch.h"
#include "hook.h"

#define err_print(fmt, args...) pr_err("%s:"fmt, __func__, ##args)

#if DEBUG == 0
# define dbg_print(fmt, args...)  ;
# define ddbg_print(fmt, args...) ;
#elif DEBUG == 1
# define dbg_print(fmt, args...) pr_info("%s:"fmt, __func__, ##args)
# define ddbg_print(fmt, args...) ;
#else /* 2 */
# define dbg_print(fmt, args...) pr_notice("%s:"fmt, __func__, ##args)
# define ddbg_print(fmt, args...) pr_info("%s:"fmt, __func__, ##args)
#endif

static bool hooked = false;
static bool registered = false;
static bool got_mmu_fault = false;
static struct omap34xxcam_videodev *last_vdev = NULL;

static struct proc_dir_entry *proc_root = NULL;

static void _camera_lines_lowpower_mode(void);
static void _camera_lines_func_mode(void);
static int camera_dev_open(struct inode *inode, struct file *file);
static int camera_dev_release(struct inode *inode, struct file *file);
static int camera_dev_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg);
static int camera_misc_probe(struct platform_device *device);
static int camera_misc_remove(struct platform_device *device);
static int cam_misc_suspend(struct platform_device *pdev, pm_message_t state);
static int cam_misc_resume(struct platform_device *pdev);
static void cam_misc_disableClk(void);
static void cam_misc_enableClk(unsigned long clock);

static int gpio_reset;
static int gpio_powerdown;
static int isp_count_local=0;
static struct regulator *regulator=NULL;
static int bHaveResetGpio;
static int bHavePowerDownGpio;

static const struct file_operations camera_dev_fops = {
	.open       = camera_dev_open,
	.release    = camera_dev_release,
	.ioctl      = camera_dev_ioctl
};

static struct miscdevice cam_misc_device0 = {
	.minor      = MISC_DYNAMIC_MINOR,
	.name       = "camera0",
	.fops       = &camera_dev_fops
};

static struct platform_driver cam_misc_driver = {
	.probe      = camera_misc_probe,
	.remove     = camera_misc_remove,
	.suspend    = cam_misc_suspend,
	.resume     = cam_misc_resume,
	.driver     = {
		.name = CAMERA_DEVICE0,
	},
};

static int camera_dev_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int camera_dev_release(struct inode *inode, struct file *file)
{
	return 0;
}

static void cam_misc_disableClk()
{
	/* powerup the isp/cam domain on the 3410 */
	if (isp_count_local == 0) {
		isp_get();
		isp_count_local++;
	}
	isp_power_settings(1);
	isp_set_xclk(0, 0);

	/* Need to make sure that all encounters of the
	   isp clocks are disabled*/
	while (isp_count_local > 0) {
		isp_put();
		isp_count_local--;
	}
}

static void cam_misc_enableClk(unsigned long clock)
{
	/* powerup the isp/cam domain on the 3410 */
	if (isp_count_local == 0) {
		isp_get();
		isp_count_local++;
	}
	isp_power_settings(1);
	isp_set_xclk(clock, 0);
}

static int cam_misc_suspend(struct platform_device *pdev, pm_message_t state)
{
	dbg_print("------------- MOT CAM SUSPEND CALLED ---------");

	/* Checking to make sure that camera is on */
	if (bHavePowerDownGpio && (gpio_get_value(gpio_powerdown) == 0)) {
		/* If camera is on, need to pull standby pin high */
		gpio_set_value(gpio_powerdown, 1);
	}

	/* Per sensor specification, need a minimum of 20 uS of time when
	   standby pin pull high and when the master clock is turned off  */
	msleep(20);
	//udelay(5000); __bad_udelay ??

	if (isp_count_local > 0) {
		/* Need to make sure that all encounters of the
		   isp clocks are disabled*/
		cam_misc_disableClk();
		dbg_print("CAMERA_MISC turned off MCLK done");
	}

	_camera_lines_lowpower_mode();

	/* Turn off power */
	if (regulator != NULL) {
		regulator_disable(regulator);
		regulator_put(regulator);
		regulator = NULL;
	}

	return 0;
}

static int cam_misc_resume(struct platform_device *pdev)
{
	return 0;
}

static int camera_dev_ioctl(struct inode *inode, struct file *file,
	unsigned int cmd, unsigned long arg)
{
	int rc = 0;

	if (!bHaveResetGpio) {
		ddbg_print("Requesting reset gpio");
		rc = gpio_request(gpio_reset, "camera reset");
		if (!rc)
			bHaveResetGpio = 1;
	}
	if (!bHavePowerDownGpio) {
		ddbg_print("Requesting powerdown gpio");
		rc = gpio_request(gpio_powerdown, "camera powerdown");
		if (!rc)
			bHavePowerDownGpio = 1;
	}
	ddbg_print("ioctl cmd %u(arg=%lu)\n", cmd, arg);

	switch (cmd) {
	case CAMERA_RESET_WRITE: //100
		if (bHaveResetGpio) {
			gpio_direction_output(gpio_reset, 0);
			gpio_set_value(gpio_reset, (arg ? 1 : 0));
			dbg_print("CAMERA_MISC set RESET line to %u\n",
				(arg ? 1 : 0));
		}

		if (!bHaveResetGpio) {
			err_print ("CAMERA_MISC: gpio_request () failed. \
				rc = %d; cmd = %d; arg = %lu", rc, cmd, arg);
			return -EIO;
		}
		break;

	case CAMERA_POWERDOWN_WRITE: //101
		if (bHavePowerDownGpio) {
			gpio_direction_output(gpio_powerdown, 0);
			if (0 == arg)
				gpio_set_value(gpio_powerdown, 0);
			else
				gpio_set_value(gpio_powerdown, 1);
			dbg_print("CAMERA_MISC set POWERDOWN line to %u\n",
				(arg ? 1 : 0));
		}
		if (!bHavePowerDownGpio) {
			err_print ("CAMERA_MISC: gpio_request () failed. \
				rc = %d; cmd = %u; arg = %lu", rc, cmd, arg);
			return -EIO;
		}
		break;

	case CAMERA_CLOCK_DISABLE: //102
		cam_misc_disableClk();
		dbg_print("CAMERA_MISC turned off MCLK done");
		break;

	case CAMERA_CLOCK_ENABLE: //103
		cam_misc_enableClk(arg);
		dbg_print("CAMERA_MISC set MCLK to %d\n", (int) arg);
		break;
	case CAMERA_AVDD_POWER_ENABLE:  //104
		arg = 1;
	case CAMERA_AVDD_POWER_DISABLE: //105
		if (arg == 1) {
			/* turn on digital power */
			if (regulator != NULL) {
				err_print("Already have regulator");
			} else {
				regulator = regulator_get(NULL, "vcam");
				if (IS_ERR(regulator)) {
					err_print("Cannot get vcam regulator, \
						err=%ld\n", PTR_ERR(regulator));
					return PTR_ERR(regulator);
				} else {
					regulator_enable(regulator);
					msleep(5);
				}
			}
			_camera_lines_func_mode();
		} else {
			/* Turn off power */
			if (regulator != NULL) {
				regulator_disable(regulator);
				regulator_put(regulator);
				regulator = NULL;
			} else {
				err_print("Regulator for vcam is not initialized");
				return -EIO;
			}
			_camera_lines_lowpower_mode();
		}
		break;
	default:
		err_print("CAMERA_MISC received unsupported cmd; cmd = %u\n",
			cmd);
		return -EIO;
		break;
	}

	return 0;
}

#define CONTROL_SYSCONFIG         0x48002010

/*
 * PADCONFS desc p. 2587 System Control Module
 * bit
 * 15  WAKEUPEVENT0
 * 14  WAKEUPENABLE0
 * 13  OFFPULLTYPESELECT0
 * 12  OFFPULLUDENABLE0
 * 11  OFFOUTVALUE0
 * 10  OFFOUTENABLE0
 * 9   OFFENABLE0
 * 8   INPUTENABLE0
 * 7:5 RESERVED(0)
 * 4   PULLTYPESELECT0
 * 3   PULLUDENABLE0
 * 2:0 MUXMODE0
 ***********************************************************/
#define CONTROL_PADCONF_CAM_XCLKA 0x48002110
#define CONTROL_PADCONF_CAM_FLD   0x48002114

/* TO CHECK: commmented padconf on working green lens */
static void _camera_lines_lowpower_mode(void)
{
	omap_writew(0x0007, 0x4800210C);   /* HSYNC CAM_HS  Line trigger input/output signal */
	omap_writew(0x0007, 0x4800210E);   /* VSYNC CAM_VS  Frame trigger input/output signal */
	omap_writew(0x001F, 0x48002110);   /* MCLK (XCLKA)  External clock for the image-sensor module */
	omap_writew(0x0007, 0x48002112);   /* PCLK          Parallel interface pixel clock */
	omap_writew(0x3A04, 0x48002114);   /* FLD           Field identification input/output signal */
//	omap_writew(0x0104, 0x48002116);   /* CAM_D0 required ? */
//	omap_writew(0x411F, 0x48002118);   /* CAM_D1 required ? */
	omap_writew(0x0007, 0x4800211A);   /* CAM_D2 required, 011F */
	omap_writew(0x0007, 0x4800211C);   /* CAM_D3 required, 011F */
	omap_writew(0x0007, 0x4800211E);   /* CAM_D4 */
	omap_writew(0x0007, 0x48002120);   /* CAM_D5 */
	omap_writew(0x0007, 0x48002122);   /* CAM_D6 */
	omap_writew(0x0007, 0x48002124);   /* CAM_D7 */
	omap_writew(0x001F, 0x48002126);   /* CAM_D8 */
	omap_writew(0x001F, 0x48002128);   /* CAM_D9 */
	omap_writew(0x001F, 0x4800212A);   /* CAM_D10 required, else green */
	omap_writew(0x001F, 0x4800212C);   /* CAM_D11 required, else green */
//	omap_writew(0x4504, 0x4800212E);   /* XCLKB          External clock for the image-sensor module */
//	omap_writew(0x011F, 0x48002130);   /* cam_wen        External write-enable signal */
//	omap_writew(0x011F, 0x48002132);   /* cam_strobe     Flash strobe control signal */
}

static void _camera_lines_func_mode(void)
{
	omap_writew(0x0118, 0x4800210C);  /* CAM_HS */
	omap_writew(0x0118, 0x4800210E);  /* CAM_VS */
	omap_writew(0x0000, 0x48002110);  /* XCLKA */
	omap_writew(0x0118, 0x48002112);  /* PCLK */
	omap_writew(0x3A04, 0x48002114);  /* FLD */
//	omap_writew(0x0104, 0x48002116);  /* CAM_D0 */
//	omap_writew(0x411F, 0x48002118);  /* CAM_D1 */
	omap_writew(0x0100, 0x4800211A);  /* CAM_D2 */
	omap_writew(0x0100, 0x4800211C);  /* CAM_D3 */
	omap_writew(0x0100, 0x4800211E);  /* CAM_D4 */
	omap_writew(0x0100, 0x48002120);  /* CAM_D5 */
	omap_writew(0x0100, 0x48002122);  /* CAM_D6 */
	omap_writew(0x0100, 0x48002124);  /* CAM_D7 */
	omap_writew(0x0100, 0x48002126);  /* CAM_D8 */
	omap_writew(0x0100, 0x48002128);  /* CAM_D9 */
	omap_writew(0x0100, 0x4800212A);  /* CAM_D10 */
	omap_writew(0x0100, 0x4800212C);  /* CAM_D11 */
//	omap_writew(0x4504, 0x4800212E);  /* XCLKB */
//	omap_writew(0x011F, 0x48002130);  /* CAM_WEN */
//	omap_writew(0x011F, 0x48002132);  /* CAM_STROBE */
}


// dump isp regs cache : cat /proc/camera/isp
static int proc_isp_read(char *page, char **start, off_t offset, int count, int *eof, void *data)
{
	int wr=0;
/*	int r;
	unsigned int state=0;

	for (r=0; r<MAX_REGS; r++) if (store->r[r].reg) {

		state = (0xff & (unsigned int) store->r[r].last_value);
		wr += scnprintf(page + wr, count - wr, REG_FMT ":%s:%04x:%x\n", (unsigned int) r, capcap_regname(r), store->r[r].mask_wr, state);
	}
*/
	if (wr <= offset + count) *eof=1;

	*start = page + offset;
	wr -= offset;
	if (wr > count) wr = count;
	if (wr < 0) wr = 0;

	return wr;
}

/* return data with "cat /proc/camera/padconf" */
static int proc_padconf_read(char *page, char **start, off_t offset, int count, int *eof, void *data)
{
	int wr=0;
	unsigned int reg;

	for (reg=0x4800210C; reg < 0x48002132; reg+=4) {
		wr += scnprintf(page + wr, count - wr, "%08x: %04hx %04hx\n",
			reg, omap_readw(reg), omap_readw(reg+2));
	}

	if (wr <= offset + count) *eof=1;
	*start = page + offset;
	wr -= offset;
	if (wr > count) wr = count;
	if (wr < 0) wr = 0;

	return wr;
}

/* PROBE / REMOVE */

static int __init camera_misc_probe(struct platform_device *device)
{
	u16 w;
	ddbg_print("device=%s", device->name);

	/* put the GPIO64 (camera powerdown) to default state
	Its getting altered by aptina (mt9p012) sensor probe */
	w = omap_readw(0x480020D0);
	if (w != 0x001C) {
		pr_notice("camera_misc: GPIO64 reg 0x%hx => 0x1c \n", w);
		w = 0x001C; omap_writew(w, 0x480020D0);
	}

	gpio_reset = get_gpio_by_name("gpio_cam_reset");
	gpio_powerdown = get_gpio_by_name("gpio_cam_pwdn");
	ddbg_print("gpio_cam_reset=%d gpio_cam_pwdn=%d", gpio_reset, gpio_powerdown);

	/* Standby needs to be high */
	if (gpio_request(gpio_powerdown, "camera powerdown") == 0) {
		gpio_direction_output(gpio_powerdown, 0);
		gpio_set_value(gpio_powerdown, 1);

		/* Do not free gpio so that it cannot be overwritten */
		bHavePowerDownGpio = 1 ;
	}
	_camera_lines_lowpower_mode();

	if (misc_register(&cam_misc_device0)) {
		err_print("error in register camera misc device!");
		return -EIO;
	}

	return 0;
}

static int camera_misc_remove(struct platform_device *device)
{
	ddbg_print("device=%s cam_misc_device0=%s\n", device->name, cam_misc_device0.name);

	cam_misc_disableClk();
	_camera_lines_lowpower_mode();

	if (bHaveResetGpio) {
		ddbg_print("free gpio_cam_reset=%d\n", gpio_reset);
		gpio_free(gpio_reset);
		bHaveResetGpio = 0;
	}
	if (bHavePowerDownGpio) {
		ddbg_print("free gpio_cam_pwdn=%d\n", gpio_powerdown);
		gpio_direction_output(gpio_powerdown, 0);
		gpio_set_value(gpio_powerdown, 1);
		gpio_free(gpio_powerdown);
		bHavePowerDownGpio = 0;
	}

	misc_deregister(&cam_misc_device0);

	/* Turn off power */
	if (regulator != NULL) {
		regulator_disable(regulator);
		regulator_put(regulator);
		regulator = NULL;
	}
	return 0;
}

/* HOOKED FUNCTIONS */

static int s_pix_parm(struct omap34xxcam_videodev *vdev,
                      struct v4l2_pix_format *best_pix_in,
                      struct v4l2_pix_format *want_pix_out,
                      struct v4l2_fract *best_ival)
{
	int rval;
	struct omap34xxcam_device* cam = vdev->cam;

	if (vdev->vfd->minor == CAM_DEVICE_SOC) { /* video3 */
		struct video_device* vfd = vdev->vfd;
		struct v4l2_device* vf2 = vfd->v4l2_dev;
		ddbg_print(" video_device=%s v4l2_device=%s\n", vfd->name, vf2->name);
		last_vdev = vdev;
	}

	// this temporary structure is not filled (see omap34xxcam.c)
	memset((void*) best_pix_in, 0, sizeof(struct v4l2_pix_format));

	/* let it do the hard work, fixit after */
	rval = HOOK_INVOKE(s_pix_parm, vdev, best_pix_in, want_pix_out, best_ival);

	ddbg_print(" cam_videodev capture_mem=0x%x index=%d ret=0x%x... \n", vdev->capture_mem, vdev->index, rval);
	if (rval) {
		/* problem happened */
		return rval;
	}

	ddbg_print(" cam_device=%p enabled=%d fck=%p ick=%p \n", cam, cam->sensor_if_enabled?1:0, cam->fck, cam->ick);
	if (best_pix_in) {
		ddbg_print("  best_pix_in w=%u h=%u sizeimage=%u fmt=%x color=%x line=%u\n",
			best_pix_in->width, best_pix_in->height, best_pix_in->sizeimage, best_pix_in->pixelformat, best_pix_in->colorspace, best_pix_in->bytesperline);
	}
	if (want_pix_out) {
		ddbg_print("  wanted_pix_out w=%u h=%u fmt=%x color=%x \n", want_pix_out->width, want_pix_out->height, want_pix_out->pixelformat, want_pix_out->colorspace);
		ddbg_print("  wanted_pix_out bytesperline=%u sizeimage=%u\n", want_pix_out->bytesperline, want_pix_out->sizeimage);
		if (want_pix_out->sizeimage != PAGE_ALIGN(want_pix_out->bytesperline * want_pix_out->height)) {
			want_pix_out->sizeimage = PAGE_ALIGN(want_pix_out->bytesperline * want_pix_out->height);
			pr_notice("sizeimage was not aligned to pagesize ! new=%u\n",want_pix_out->sizeimage);
		}
		/* according to v4l2, bytesperline can be set to 0, as padding */
		want_pix_out->bytesperline = 0; //want_pix_out->width * 2;
	}
	if (best_ival) {
		ddbg_print("  best_ival=%u/%u\n", best_ival->numerator, best_ival->denominator);
	}
	return rval;
}

/*
 * struct ispccdc_syncif - Structure for Sync Interface between sensor and CCDC
 */
#if 0
void _ispccdc_config_sync_if(struct ispccdc_syncif syncif)
{
	ddbg_print("syncif datsz=%d ipmod=%d hdpol=%hu vdpol=%hu\n",
		syncif.datsz, syncif.ipmod, syncif.hdpol, syncif.vdpol);

	if (syncif.datsz == DAT8 && syncif.ipmod == YUV16 && syncif.vdpol == 1) {
		//ddbg_print("set syncif.vdpol to 0\n");
		//syncif.vdpol = 0;
		//syncif.hdpol = 1;
	}
	HOOK_INVOKE(ispccdc_config_sync_if, syncif);
}
#endif

SYMSEARCH_DECLARE_FUNCTION_STATIC(
	void, ss_omapvout_chk_overrides, struct omapvout_device *vout);

extern int cam_dss_acquire_vrfb(struct omapvout_device *vout);

int omapvout_open(struct file *file)
{
	struct omapvout_device *vout;
	u16 w, h;
	int rc;

	if (file) ddbg_print(" file=%p\n", &file->f_path);

	vout = video_drvdata(file);

	if (vout == NULL) {
		pr_err("Invalid device\n");
		return -ENODEV;
	}

	mutex_lock(&vout->mtx);

	/* We only support single open */
	if (vout->opened) {
		ddbg_print(": Device already opened\n");
		cam_dss_release(vout);
		rc = -EBUSY;
		goto failed;
	}

	//cam_dss_remove(vout);
	//cam_dss_acquire_vrfb(vout);
	//cam_dss_init(vout,);
	rc = cam_dss_open(vout, &w, &h);
	if (rc != 0)
		goto failed;

	dbg_print("Overlay Display w/h %dx%d\n", w, h);

	if (w == 0 || h == 0) {
		pr_err("Invalid display resolution\n");
		rc = -EINVAL;
		goto failed;
	}

	rc = cam_vbq_init(vout);
	if (rc != 0)
		goto failed;

	vout->disp_width = w;
	vout->disp_height = h;
	vout->opened = 1;

	memset(&vout->pix, 0, sizeof(vout->pix));
	vout->pix.width = w;
	vout->pix.height = h;
	vout->pix.field = V4L2_FIELD_NONE;
	vout->pix.pixelformat = V4L2_PIX_FMT_RGB565; /* Arbitrary */
	vout->pix.colorspace = V4L2_COLORSPACE_SRGB; /* Arbitrary */
	vout->pix.bytesperline = w * ISP_BYTES_PER_PIXEL;
	vout->pix.sizeimage = PAGE_ALIGN(vout->pix.bytesperline * h);
	vout->pix.priv = 0;

	memset(&vout->win, 0, sizeof(vout->win));
	vout->win.w.width = w;
	vout->win.w.height = h;
	vout->win.field = V4L2_FIELD_NONE;

	memset(&vout->crop, 0, sizeof(vout->crop));
	vout->crop.width = w;
	vout->crop.height = h;

	memset(&vout->fbuf, 0, sizeof(vout->fbuf));
	vout->rotation = 0;
	vout->bg_color = 0;
	vout->mmap_cnt = 0;

	SYMSEARCH_BIND_FUNCTION_TO(camera, omapvout_chk_overrides, ss_omapvout_chk_overrides);
	ss_omapvout_chk_overrides(vout);

	mutex_unlock(&vout->mtx);

	file->private_data = vout;

	ddbg_print("done");
	return 0;

failed:
	printk(KERN_ERR "fail !\n");
	mutex_unlock(&vout->mtx);
	return rc;
}

#if 0
static void _omap_set_dma_src_burst_mode(int lch, enum omap_dma_burst_mode burst_mode) {
	pr_notice("%s(%d,%d)\n",__func__, lch, burst_mode);
	HOOK_INVOKE(omap_set_dma_src_burst_mode, lch, OMAP_DMA_DATA_BURST_16);
}
static void _omap_set_dma_dest_burst_mode(int lch, enum omap_dma_burst_mode burst_mode) {
	pr_notice("%s(%d,%d)\n",__func__, lch, burst_mode);
	HOOK_INVOKE(omap_set_dma_dest_burst_mode, lch, OMAP_DMA_DATA_BURST_16);
}
static void _omap_set_dma_dest_params(int lch, int dest_port, int dest_amode, unsigned long dest_start, int dst_ei, int dst_fi) {
//	pr_info("%s(%d,%d,%d,0x%lx,%d,%d)\n",__func__, lch, dest_port, dest_amode, dest_start, dst_ei, dst_fi);
	HOOK_INVOKE(omap_set_dma_dest_params, lch, dest_port, dest_amode, dest_start, dst_ei, dst_fi);
	return;
}
#endif

/*
 * 6.4.11.1 Camera ISP MMU Features (p. 1256)
 *	The camera ISP MMU contains a translation lookaside buffer (TLB) that holds translations and properties
 *	for current pages. This TLB can be managed statically through the configuration slave port, or by the
 *	internal hardware table-walking logic (TWL), which can autonomously traverse the page table on a TLB
 *	miss. The TWL can be enabled or disabled (MMU.MMU_CNTL[2] TWLENABLE).
 *	On a TLB miss, the initiator is stalled until a valid address translation is found. If no valid translation is
 *	found, a translation fault interrupt is generated to the processor. It is a nonrecoverable situation, which
 *	leads to the camera ISP being reset by the processor software.
 *	The MMU provides up to 4G bytes of virtual memory.
 *
 * 6.4.11.2 Camera ISP MMU Functional Description
 *	For a detailed description of the MMU, see Chapter 15, Memory Management Units.
 *	NOTE: In the camera ISP MMU, the endianess feature is available for write, but conversion for read
 *	is not possible.
 */

/* from iommu2.c */
#define MMU_SYS_IDLE_SHIFT  3 /* SYSCONF */
#define MMU_SYS_IDLE_FORCE  (0 << MMU_SYS_IDLE_SHIFT)
#define MMU_CNTL_SHIFT      1 /* MMU_CNTL */
#define MMU_CNTL_MASK       (7 << MMU_CNTL_SHIFT)
/* IRQSTATUS & IRQENABLE */
#define MMU_IRQ_MULTIHITFAULT   (1 << 4)
#define MMU_IRQ_TABLEWALKFAULT  (1 << 3)
#define MMU_IRQ_EMUMISS     (1 << 2)
#define MMU_IRQ_TRANSLATIONFAULT    (1 << 1)
#define MMU_IRQ_TLBMISS     (1 << 0)
#define MMU_IRQ_MASK    \
    (MMU_IRQ_MULTIHITFAULT | MMU_IRQ_TABLEWALKFAULT | MMU_IRQ_EMUMISS | \
     MMU_IRQ_TRANSLATIONFAULT)

/* no real hook here, but a substitute */
static u32 _omap2_iommu_fault_isr(struct iommu *obj, u32 *ra)
{
	u32 ret = 0, stat, da = 0;
	u32 errs = 0;
	stat = iommu_read_reg(obj, MMU_IRQSTATUS);
	stat &= MMU_IRQ_MASK;
	if (!stat) {
		*ra = 0;
		return 0;
	} else {
		da = iommu_read_reg(obj, MMU_FAULT_AD);
		*ra = da;

		if (stat & MMU_IRQ_TLBMISS)
			errs |= MMU_IRQ_TLBMISS;
		if (stat & MMU_IRQ_TRANSLATIONFAULT)
			errs |= MMU_IRQ_TRANSLATIONFAULT;
		if (stat & MMU_IRQ_EMUMISS)
			errs |= MMU_IRQ_EMUMISS;
		if (stat & MMU_IRQ_TABLEWALKFAULT)
			errs |= MMU_IRQ_TABLEWALKFAULT;
		if (stat & MMU_IRQ_MULTIHITFAULT)
			errs |= MMU_IRQ_MULTIHITFAULT;

		/* reset irq error, warning: bits to 1 to reset to 0*/
		iommu_write_reg(obj, stat, MMU_IRQSTATUS);
	}
	pr_notice("%s(%p,%p=>%x)=%d\n",__func__, obj, ra, *ra, ret);
	if (stat == MMU_IRQ_TRANSLATIONFAULT && !strcmp("cam_ick",obj->clk->name)) { /* translation fault */
		int cdcc = isp_reg_readl(OMAP3_ISP_IOMEM_CCDC, ISPCCDC_PCR) & ISPCCDC_PCR_BUSY;
		int prv  = isp_reg_readl(OMAP3_ISP_IOMEM_PREV, ISPPRV_PCR)  & ISPPRV_PCR_BUSY;
		int sbl = ispccdc_sbl_busy();
		pr_warning("%s: got_mmu_fault ispccdc_busy=%x, sbl_busy=%x, preview_busy=%x\n", __func__, cdcc, sbl, prv);
		if (sbl) {
			pr_info(" ISPSBL_CCDC_WR_0 is now %x\n", isp_reg_readl(OMAP3_ISP_IOMEM_SBL, ISPSBL_CCDC_WR_0));
			pr_info(" ISPSBL_CCDC_WR_1 is now %x\n", isp_reg_readl(OMAP3_ISP_IOMEM_SBL, ISPSBL_CCDC_WR_1));
			pr_info(" ISPSBL_CCDC_WR_2 is now %x\n", isp_reg_readl(OMAP3_ISP_IOMEM_SBL, ISPSBL_CCDC_WR_2));
			pr_info(" ISPSBL_CCDC_WR_3 is now %x\n", isp_reg_readl(OMAP3_ISP_IOMEM_SBL, ISPSBL_CCDC_WR_3));

			/* look like fully broken when we have this dest address */
			ret = iommu_read_reg(obj, MMU_CNTL); // returns 0x6 (TWLENABLE|MMUENABLE)
			*ra = 0;

			pr_warning("MMU_CNTL=%x shutdown %s\n", ret, dev_name(obj->dev));
			// disable MMU
			iommu_write_reg(obj, ret & ~MMU_CNTL_MASK, MMU_CNTL);
			iommu_write_reg(obj, MMU_SYS_IDLE_FORCE, MMU_SYSCONFIG);

			got_mmu_fault = true;
		}
	}
	return errs;
}

static int omap34xxcam_vbq_prepare(struct videobuf_queue *vbq,
                                   struct videobuf_buffer *vb,
                                   enum v4l2_field field)
{
	int ret;
	ret = HOOK_INVOKE(omap34xxcam_vbq_prepare, vbq, vb, field);

	pr_notice("%s(queue=%p,buffer=%d,field=%x) ret=%d\n", __func__, vbq, vb->i, field, ret);
	pr_info("  videobuf_buffer: width=%u height=%u bytesperline=%u\n", vb->width, vb->height, vb->bytesperline);
	pr_info("                   size=%lu state=%d field_count=%u\n", vb->size, vb->state, vb->field_count);
	pr_info("                   buffer userland ptr=%lx bsize=%d boffset=%d\n", vb->baddr, vb->bsize, vb->boff);
	// vb->input is always 0xFFFFFFFF
	// vb->map is not filled here

	// TODO: vb->size=0 unlike froyo kernel which has the same value as vb->bsize
	return ret;
}

struct hook_info g_hi[] = {
	HOOK_INIT(s_pix_parm), // memset last_vdev
	HOOK_INIT(omap34xxcam_vbq_prepare), // log purpose
	HOOK_RENAME(omap2_iommu_fault_isr,_omap2_iommu_fault_isr), // to stop mmu on error
	HOOK_INIT(omapvout_open),
//	HOOK_RENAME(ispccdc_config_sync_if, _ispccdc_config_sync_if),
//	HOOK_RENAME(omap_set_dma_src_burst_mode, _omap_set_dma_src_burst_mode), /* never called */
//	HOOK_RENAME(omap_set_dma_dest_params, _omap_set_dma_dest_params),
//	HOOK_RENAME(omap_set_dma_dest_burst_mode, _omap_set_dma_dest_burst_mode),
	HOOK_INIT_END
};

/* UNREGISTER/FREE HP/3A (Red Lens Camera)
SYMSEARCH_DECLARE_FUNCTION_STATIC(void, ss_deinitialize_hp3a_framework, void);

static int cam_hp3a_remove(struct platform_device *device)
{
	SYMSEARCH_BIND_FUNCTION_TO(camera, deinitialize_hp3a_framework, ss_deinitialize_hp3a_framework);
	// Deinitialize 3A task, this is a blocking call.
	ss_deinitialize_hp3a_framework();
	return 0;
}
static struct platform_driver cam_hp3a_driver = {
	.remove = cam_hp3a_remove,
	.driver = { .name = "hp3a" },
};
*/

/* in board_defs.c */
extern int  camise_add_i2c_device(void);
extern void camise_del_i2c_device(void);
extern void hplens_i2c_release(void);
extern void aptina_i2c_release(void);

static int __init camera_misc_init(void)
{
	int ret=0;
	struct proc_dir_entry *proc_entry;
	unsigned int reg;

	ddbg_print("unregister aptina i2c interface\n");
	aptina_i2c_release();
	ddbg_print("unregister red lens i2c interface\n");
	hplens_i2c_release();
	//ddbg_print("unregister hp3a_driver\n");
	//platform_driver_unregister(&cam_hp3a_driver);

	// to prevent the gpio error on camera open
	if (omap_readw(0x480020D0) != 0x001C) {
		pr_notice("camera_misc: restore GPIO64 config, altered by mt9p012 driver\n");
		omap_writew(0x001C, 0x480020D0);
	}

	// print padconf
	pr_notice("camera padconf:\n");
	for (reg=0x4800210C; reg < 0x48002132; reg += 4) {
		pr_info(" %08x: %04hx %04hx\n", reg, omap_readw(reg), omap_readw(reg+2));
	}

	hook_init();
	hooked = true;

	ret = camise_add_i2c_device();
	registered = (ret == 0);
	if (registered) {
		ret=platform_driver_register(&cam_misc_driver);
	} else {
		pr_info("%s: This module is not for froyo kernel, exiting\n", __func__);
		hook_exit();
		hooked = false;
		return -EEXIST;
	}

	proc_root = proc_mkdir(TAG, NULL);
	proc_entry = create_proc_read_entry("isp", 0440, proc_root, proc_isp_read, NULL);
	proc_entry = create_proc_read_entry("padconf", 0440, proc_root, proc_padconf_read, NULL);
	//proc_entry->write_proc = ;

	return ret;
}

static void __exit camera_misc_exit(void)
{
	remove_proc_entry("isp", proc_root);
	remove_proc_entry("padconf", proc_root);
	remove_proc_entry(TAG, NULL);

	if (hooked) hook_exit();
	if (last_vdev) {
		// for omap34xxcam_release
		pr_info(TAG": reset users=%d, slaves=%d ",  last_vdev->slaves, atomic_read(&last_vdev->users));
		atomic_set(&last_vdev->users,0);
		last_vdev->vfd->release(last_vdev->vfd);
		last_vdev = NULL;
	}
	if (registered) {
		platform_driver_unregister(&cam_misc_driver);
		camise_del_i2c_device();
	}
}

module_init(camera_misc_init);
module_exit(camera_misc_exit);

MODULE_VERSION("1.4");
MODULE_DESCRIPTION("Motorola Camera Driver Backport");
MODULE_AUTHOR("Tanguy Pruvot, From Motorola Open Sources");
MODULE_LICENSE("GPL");
