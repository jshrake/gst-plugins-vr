/*
 * GStreamer Plugins VR
 * Copyright (C) 2016 Lubosz Sarnecki <lubosz@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-freenect2src
 *
 * <refsect2>
 * <title>Examples</title>
 * <para>
 * <programlisting>
  gst-launch-1.0 freenect2src sourcetype=0 ! videoconvert ! glimagesink
 * </programlisting>
 * </para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "gstfreenect2src.h"

GST_DEBUG_CATEGORY_STATIC (freenect2src_debug);
#define GST_CAT_DEFAULT freenect2src_debug
static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{RGBA, RGB, GRAY16_LE}"))
    );

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_SOURCETYPE
};
typedef enum
{
  SOURCETYPE_DEPTH,
  SOURCETYPE_COLOR,
  SOURCETYPE_IR,
  SOURCETYPE_COLOR_DEPTH,
} GstFreenect2SourceType;
#define DEFAULT_SOURCETYPE  SOURCETYPE_DEPTH
#define GST_TYPE_FREENECT2_SRC_SOURCETYPE (gst_freenect2_src_sourcetype_get_type ())
static GType
gst_freenect2_src_sourcetype_get_type (void)
{
  static GType etype = 0;
  if (etype == 0) {
    static const GEnumValue values[] = {
      {SOURCETYPE_DEPTH, "Get depth readings", "depth"},
      {SOURCETYPE_COLOR, "Get color readings", "color"},
      {SOURCETYPE_IR, "Get color readings", "ir"},
      {SOURCETYPE_COLOR_DEPTH, "Get color and depth", "color_depth"},
      {0, NULL, NULL},
    };
    etype = g_enum_register_static ("GstFreenect2SrcSourcetype", values);
  }
  return etype;
}

/* GObject methods */
static void gst_freenect2_src_dispose (GObject * object);
static void gst_freenect2_src_finalize (GObject * gobject);
static void gst_freenect2_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_freenect2_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

/* basesrc methods */
static gboolean gst_freenect2_src_start (GstBaseSrc * bsrc);
static gboolean gst_freenect2_src_stop (GstBaseSrc * bsrc);
static gboolean gst_freenect2_src_set_caps (GstBaseSrc * src, GstCaps * caps);
static GstCaps *gst_freenect2_src_get_caps (GstBaseSrc * src, GstCaps * filter);
static gboolean gst_freenect2src_decide_allocation (GstBaseSrc * bsrc,
    GstQuery * query);

/* element methods */
static GstStateChangeReturn gst_freenect2_src_change_state (GstElement *
    element, GstStateChange transition);

/* pushsrc method */
static GstFlowReturn gst_freenect2src_fill (GstPushSrc * src, GstBuffer * buf);

/* OpenNI2 interaction methods */
static gboolean freenect2_initialise_devices (GstFreenect2Src * src);
static GstFlowReturn freenect2_read_gstbuffer (GstFreenect2Src * src,
    GstBuffer * buf);

#define parent_class gst_freenect2_src_parent_class
G_DEFINE_TYPE (GstFreenect2Src, gst_freenect2_src, GST_TYPE_PUSH_SRC);

static void
gst_freenect2_src_class_init (GstFreenect2SrcClass * klass)
{
  GObjectClass *gobject_class;
  GstPushSrcClass *pushsrc_class;
  GstBaseSrcClass *basesrc_class;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class = (GObjectClass *) klass;
  basesrc_class = (GstBaseSrcClass *) klass;
  pushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->dispose = gst_freenect2_src_dispose;
  gobject_class->finalize = gst_freenect2_src_finalize;
  gobject_class->set_property = gst_freenect2_src_set_property;
  gobject_class->get_property = gst_freenect2_src_get_property;
  g_object_class_install_property
      (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "Location",
          "Source uri, can be a file or a device.", "", (GParamFlags)
          (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_SOURCETYPE,
      g_param_spec_enum ("sourcetype",
          "Device source type",
          "Type of readings to get from the source",
          GST_TYPE_FREENECT2_SRC_SOURCETYPE, DEFAULT_SOURCETYPE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));


  basesrc_class->start = GST_DEBUG_FUNCPTR (gst_freenect2_src_start);
  basesrc_class->stop = GST_DEBUG_FUNCPTR (gst_freenect2_src_stop);
  basesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_freenect2_src_get_caps);
  basesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_freenect2_src_set_caps);
  basesrc_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_freenect2src_decide_allocation);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&srctemplate));

  gst_element_class_set_static_metadata (element_class,
      "Freenect2 client source", "Source/Video",
      "Extract readings from an Kinect v2. ",
      "Lubosz Sarnecki <lubosz@collabora.co.uk>");

  element_class->change_state = gst_freenect2_src_change_state;

  pushsrc_class->fill = GST_DEBUG_FUNCPTR (gst_freenect2src_fill);

  GST_DEBUG_CATEGORY_INIT (freenect2src_debug, "freenect2src", 0,
      "Freenect2 Device Source");
}

static void
gst_freenect2_src_init (GstFreenect2Src * self)
{
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  self->dev = NULL;
  self->listener = NULL;
  self->freenect2 = new libfreenect2::Freenect2 ();
}

static void
gst_freenect2_src_dispose (GObject * object)
{
  GstFreenect2Src *self = GST_FREENECT2_SRC (object);
  if (self->gst_caps)
    gst_caps_unref (self->gst_caps);
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_freenect2_src_finalize (GObject * gobject)
{
  GstFreenect2Src *self = GST_FREENECT2_SRC (gobject);

  if (self->uri_name) {
    g_free (self->uri_name);
    self->uri_name = NULL;
  }

  if (self->gst_caps) {
    gst_caps_unref (self->gst_caps);
    self->gst_caps = NULL;
  }

  self->dev->close ();
  delete self->listener;
  // delete freenect2src->registration;
  //delete self->dev;
  //if (self->freenect2 != NULL)
  //  delete self->freenect2;

  G_OBJECT_CLASS (parent_class)->finalize (gobject);
}

static void
gst_freenect2_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstFreenect2Src *self = GST_FREENECT2_SRC (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_LOCATION:
      if (!g_value_get_string (value)) {
        GST_WARNING ("location property cannot be NULL");
        break;
      }
      if (self->uri_name != NULL) {
        g_free (self->uri_name);
        self->uri_name = NULL;
      }
      self->uri_name = g_value_dup_string (value);
      break;
    case PROP_SOURCETYPE:
      self->sourcetype = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_freenect2_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstFreenect2Src *self = GST_FREENECT2_SRC (object);
  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_LOCATION:
      g_value_set_string (value, self->uri_name);
      break;
    case PROP_SOURCETYPE:
      g_value_set_enum (value, self->sourcetype);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}


static gboolean
gst_freenect2_src_start (GstBaseSrc * bsrc)
{
  return TRUE;
}

static gboolean
gst_freenect2_src_stop (GstBaseSrc * bsrc)
{
  GstFreenect2Src *self = GST_FREENECT2_SRC (bsrc);
  self->dev->stop ();
  return TRUE;
}

static GstCaps *
gst_freenect2_src_get_caps (GstBaseSrc * src, GstCaps * filter)
{
  GstCaps *caps;
  GstVideoInfo info;
  GstVideoFormat format;

  GstFreenect2Src *self = GST_FREENECT2_SRC (src);

  GST_OBJECT_LOCK (self);
  if (self->gst_caps)
    goto out;

  // If we are here, we need to compose the caps and return them.

  gst_video_info_init (&info);

  if (self->sourcetype == SOURCETYPE_COLOR_DEPTH) {
    format = GST_VIDEO_FORMAT_RGBA;
    gst_video_info_set_format (&info, format, 1920, 1080);
  } else if (self->sourcetype == SOURCETYPE_DEPTH
      || self->sourcetype == SOURCETYPE_IR) {
    format = GST_VIDEO_FORMAT_GRAY16_LE;
    //format = GST_VIDEO_FORMAT_GRAY32F;
    gst_video_info_set_format (&info, format, 512, 424);
  } else if (self->sourcetype == SOURCETYPE_COLOR) {
    format = GST_VIDEO_FORMAT_RGB;
    gst_video_info_set_format (&info, format, 1920, 1080);
  } else {
    goto out;
  }


  //gst_video_info_set_format (&info, format, freenect2src->width, freenect2src->height);
  //info.fps_n = freenect2src->fps;
  info.fps_n = 30;
  info.fps_d = 1;
  caps = gst_video_info_to_caps (&info);

  GST_DEBUG_OBJECT (self, "probed caps: %" GST_PTR_FORMAT, caps);
  self->gst_caps = caps;

out:
  GST_OBJECT_UNLOCK (self);

  if (!self->gst_caps)
    return gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (self));

  return (filter)
      ? gst_caps_intersect_full (filter, self->gst_caps,
      GST_CAPS_INTERSECT_FIRST)
      : gst_caps_ref (self->gst_caps);
}

static gboolean
gst_freenect2_src_set_caps (GstBaseSrc * src, GstCaps * caps)
{
  GstFreenect2Src *self = GST_FREENECT2_SRC (src);
  return gst_video_info_from_caps (&self->info, caps);
}

static GstStateChangeReturn
gst_freenect2_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_FAILURE;
  GstFreenect2Src *self = GST_FREENECT2_SRC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!freenect2_initialise_devices (self))
        return GST_STATE_CHANGE_FAILURE;
      break;
    default:
      break;
  }
  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    return ret;
  }

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_freenect2_src_stop (GST_BASE_SRC (self));
      if (self->gst_caps) {
        gst_caps_unref (self->gst_caps);
        self->gst_caps = NULL;
      }
      break;
    default:
      break;
  }
  return ret;
}


static GstFlowReturn
gst_freenect2src_fill (GstPushSrc * src, GstBuffer * buf)
{
  GstFreenect2Src *self = GST_FREENECT2_SRC (src);
  return freenect2_read_gstbuffer (self, buf);
}

static gboolean
gst_freenect2src_decide_allocation (GstBaseSrc * bsrc, GstQuery * query)
{
  GstBufferPool *pool;
  guint size, min, max;
  gboolean update;
  GstStructure *config;
  GstCaps *caps;
  GstVideoInfo info;

  gst_query_parse_allocation (query, &caps, NULL);
  gst_video_info_from_caps (&info, caps);

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
    update = TRUE;
  } else {
    pool = NULL;
    min = max = 0;
    size = info.size;
    update = FALSE;
  }

  GST_DEBUG_OBJECT (bsrc, "allocation: size:%u min:%u max:%u pool:%"
      GST_PTR_FORMAT " caps:%" GST_PTR_FORMAT, size, min, max, pool, caps);

  if (!pool)
    pool = gst_video_buffer_pool_new ();

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, min, max);

  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)) {
    GST_DEBUG_OBJECT (pool, "activate Video Meta");
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
  }

  gst_buffer_pool_set_config (pool, config);

  if (update)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  gst_object_unref (pool);

  return GST_BASE_SRC_CLASS (parent_class)->decide_allocation (bsrc, query);
}


static gboolean
freenect2_initialise_devices (GstFreenect2Src * self)
{
  if (self->freenect2->enumerateDevices () == 0) {
    GST_ERROR ("no device connected!");
    return FALSE;
  }

  std::string serial = self->freenect2->getDefaultDeviceSerialNumber ();

  GST_DEBUG ("freenect2 device serial %s", serial.c_str ());

  self->dev = self->freenect2->openDevice (serial);

  if (self->dev == 0) {
    GST_ERROR ("failure opening device!");
    return FALSE;
  }

  if (self->sourcetype == SOURCETYPE_COLOR_DEPTH) {
    self->listener =
        new libfreenect2::SyncMultiFrameListener (libfreenect2::
        Frame::Color | libfreenect2::Frame::Depth);
    self->dev->setColorFrameListener (self->listener);
    self->dev->setIrAndDepthFrameListener (self->listener);
  } else if (self->sourcetype == SOURCETYPE_DEPTH) {
    self->listener =
        new libfreenect2::SyncMultiFrameListener (libfreenect2::Frame::Depth);
    self->dev->setIrAndDepthFrameListener (self->listener);
  } else if (self->sourcetype == SOURCETYPE_IR) {
    self->listener =
        new libfreenect2::SyncMultiFrameListener (libfreenect2::Frame::Ir);
    self->dev->setIrAndDepthFrameListener (self->listener);
  } else if (self->sourcetype == SOURCETYPE_COLOR) {
    self->listener =
        new libfreenect2::SyncMultiFrameListener (libfreenect2::Frame::Color);
    self->dev->setColorFrameListener (self->listener);
  }

/*
  self->undistorted = new libfreenect2::Frame(512, 424, 4);
  self->registered = new libfreenect2::Frame(512, 424, 4);
*/

  self->dev->start ();

  GST_DEBUG ("device serial: %s", self->dev->getSerialNumber ().c_str ());
  GST_DEBUG ("device firmware: %s", self->dev->getFirmwareVersion ().c_str ());

/*
  self->registration = new libfreenect2::Registration(
    self->dev->getIrCameraParams(),
    self->dev->getColorCameraParams());
*/

  return TRUE;
}

static GstFlowReturn
freenect2_read_gstbuffer (GstFreenect2Src * self, GstBuffer * buf)
{
  GstVideoFrame vframe;
  libfreenect2::Frame * ir = NULL;
  libfreenect2::Frame * rgb = NULL;
  libfreenect2::Frame * depth = NULL;
  self->listener->waitForNewFrame (self->frames);

  gst_video_frame_map (&vframe, &self->info, buf, GST_MAP_WRITE);

  if (self->sourcetype == SOURCETYPE_COLOR_DEPTH) {
    rgb = self->frames[libfreenect2::Frame::Color];
    depth = self->frames[libfreenect2::Frame::Depth];

    guint8 *pData = (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0);
    guint8 *pColor = (guint8 *) rgb->data;
    float *pDepth = (float *) depth->data;

    for (unsigned j = 0; j < rgb->height; ++j) {
      for (unsigned i = 0; i < rgb->width; ++i) {

        pData[4 * i + 2] = pColor[4 * i + 0];
        pData[4 * i + 1] = pColor[4 * i + 1];
        pData[4 * i + 0] = pColor[4 * i + 2];
        if (i < depth->width && j < depth->height) {
          unsigned index = depth->width * j + i;
          pData[4 * i + 3] = (guint8) pDepth[index];
        } else {
          pData[4 * i + 3] = 255;
        }

      }
      pData += GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0);
      pColor += rgb->bytes_per_pixel * rgb->width;
      //pDepth += sizeof(float) * rgb->width;
    }
  } else if (self->sourcetype == SOURCETYPE_DEPTH) {
    depth = self->frames[libfreenect2::Frame::Depth];
    guint16 *pData = (guint16 *) GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0);
    gfloat *pDepth = (float *) depth->data;

    //TODO: use 16-bit float buffers.
    for (unsigned i = 0; i < depth->height * depth->width; ++i) {
      float mapped_float = pDepth[i] * 65.535f/4.0;
      pData[i] = (guint16) mapped_float;
    }

  } else if (self->sourcetype == SOURCETYPE_IR) {
    ir = self->frames[libfreenect2::Frame::Ir];
    guint16 *pData = (guint16 *) GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0);
    float *pDepth = (float *) ir->data;
    for (unsigned i = 0; i < ir->height * ir->width; ++i)
      pData[i] = (guint16) pDepth[i];
  } else if (self->sourcetype == SOURCETYPE_COLOR) {
    rgb = self->frames[libfreenect2::Frame::Color];
    guint8 *pData = (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0);
    guint8 *pColor = (guint8 *) rgb->data;
    // Add depth as 8bit alpha channel, depth is 16bit samples.
    //guint16 *pDepth = (guint16 *) self->depthFrame->getData ();

    for (unsigned j = 0; j < rgb->height; ++j) {
      for (unsigned i = 0; i < rgb->width; ++i) {

        pData[3 * i + 2] = pColor[4 * i + 0];
        pData[3 * i + 1] = pColor[4 * i + 1];
        pData[3 * i + 0] = pColor[4 * i + 2];
        //pData[4 * j + 3] = pDepth[j] >> 8;
      }
      pData += GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0);
      pColor += rgb->bytes_per_pixel * rgb->width;
    }

  }
  gst_video_frame_unmap (&vframe);
  self->listener->release (self->frames);

  return GST_FLOW_OK;
}
