/*
 * drivers/media/video/omap/omapvout-dss.h
 *
 * Copyright (C) 2009 Motorola Inc.
 *
 * Based on drivers/media/video/omap24xx/omap24xxvout.c&h
 *
 * Copyright (C) 2005-2006 Texas Instruments.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#ifndef __CAM_OMAPVOUT_DSS_H__
#define __CAM_OMAPVOUT_DSS_H__

/* Driver init/remove time calls */
extern int  cam_dss_init(struct omapvout_device *vout,
						enum omap_plane plane);
extern void cam_dss_remove(struct omapvout_device *vout);

/* Driver open/release time calls */
extern int  cam_dss_open(struct omapvout_device *vout,
						u16 *disp_w, u16 *disp_h);
extern void cam_dss_release(struct omapvout_device *vout);

/* Driver operation calls */
extern bool cam_dss_is_rotation_supported(struct omapvout_device *vout);
extern int  cam_dss_enable(struct omapvout_device *vout);
extern void cam_dss_disable(struct omapvout_device *vout);
extern int  cam_dss_update(struct omapvout_device *vout);

#endif /* __CAM_OMAPVOUT_DSS_H__ */

