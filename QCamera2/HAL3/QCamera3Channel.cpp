/* Copyright (c) 2012-2014, The Linux Foundataion. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#define LOG_TAG "QCamera3Channel"
//#define LOG_NDEBUG 0

#include <stdlib.h>
#include <cstdlib>
#include <stdio.h>
#include <string.h>
#include <hardware/camera3.h>
#include <system/camera_metadata.h>
#include <gralloc_priv.h>
#include <utils/Log.h>
#include <utils/Errors.h>
#include <cutils/properties.h>
#include "QCamera3Channel.h"

using namespace android;

#define MIN_STREAMING_BUFFER_NUM 7+11

namespace qcamera {
static const char ExifAsciiPrefix[] =
    { 0x41, 0x53, 0x43, 0x49, 0x49, 0x0, 0x0, 0x0 };          // "ASCII\0\0\0"
static const char ExifUndefinedPrefix[] =
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };   // "\0\0\0\0\0\0\0\0"

#define GPS_PROCESSING_METHOD_SIZE       101
#define EXIF_ASCII_PREFIX_SIZE           8   //(sizeof(ExifAsciiPrefix))
#define FOCAL_LENGTH_DECIMAL_PRECISION   100

/*===========================================================================
 * FUNCTION   : QCamera3Channel
 *
 * DESCRIPTION: constrcutor of QCamera3Channel
 *
 * PARAMETERS :
 *   @cam_handle : camera handle
 *   @cam_ops    : ptr to camera ops table
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3Channel::QCamera3Channel(uint32_t cam_handle,
                               mm_camera_ops_t *cam_ops,
                               channel_cb_routine cb_routine,
                               cam_padding_info_t *paddingInfo,
                               void *userData)
{
    m_camHandle = cam_handle;
    m_camOps = cam_ops;
    m_bIsActive = false;

    m_handle = 0;
    m_numStreams = 0;
    memset(mStreams, 0, sizeof(mStreams));
    mUserData = userData;

    mStreamInfoBuf = NULL;
    mChannelCB = cb_routine;
    mPaddingInfo = paddingInfo;

    mPostProcMask = postprocess_mask;

    char prop[PROPERTY_VALUE_MAX];
    property_get("persist.camera.yuv.dump", prop, "0");
    mYUVDump = (uint8_t) atoi(prop);

}

/*===========================================================================
 * FUNCTION   : QCamera3Channel
 *
 * DESCRIPTION: default constrcutor of QCamera3Channel
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3Channel::QCamera3Channel()
{
    m_camHandle = 0;
    m_camOps = NULL;
    m_bIsActive = false;

    m_handle = 0;
    m_numStreams = 0;
    memset(mStreams, 0, sizeof(mStreams));
    mUserData = NULL;

    mStreamInfoBuf = NULL;
    mChannelCB = NULL;
    mPaddingInfo = NULL;
}

/*===========================================================================
 * FUNCTION   : ~QCamera3Channel
 *
 * DESCRIPTION: destructor of QCamera3Channel
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3Channel::~QCamera3Channel()
{
    if (m_bIsActive)
        stop();

    for (uint32_t i = 0; i < m_numStreams; i++) {
        if (mStreams[i] != NULL) {
            delete mStreams[i];
            mStreams[i] = 0;
        }
    }
    if (m_handle) {
        m_camOps->delete_channel(m_camHandle, m_handle);
        ALOGE("%s: deleting channel %d", __func__, m_handle);
        m_handle = 0;
    }
    m_numStreams = 0;
}

/*===========================================================================
 * FUNCTION   : init
 *
 * DESCRIPTION: initialization of channel
 *
 * PARAMETERS :
 *   @attr    : channel bundle attribute setting
 *   @dataCB  : data notify callback
 *   @userData: user data ptr
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3Channel::init(mm_camera_channel_attr_t *attr,
                             mm_camera_buf_notify_t dataCB)
{
    m_handle = m_camOps->add_channel(m_camHandle,
                                      attr,
                                      dataCB,
                                      this);
    if (m_handle == 0) {
        ALOGE("%s: Add channel failed", __func__);
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : addStream
 *
 * DESCRIPTION: add a stream into channel
 *
 * PARAMETERS :
 *   @streamType     : stream type
 *   @streamFormat   : stream format
 *   @streamDim      : stream dimension
 *   @minStreamBufNum : minimal buffer count for particular stream type
 *   @postprocess_mask : post-proccess feature mask
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3Channel::addStream(cam_stream_type_t streamType,
                                  cam_format_t streamFormat,
                                  cam_dimension_t streamDim,
                                  uint8_t minStreamBufNum)
{
    int32_t rc = NO_ERROR;

    if (m_numStreams >= 1) {
        ALOGE("%s: Only one stream per channel supported in v3 Hal", __func__);
        return BAD_VALUE;
    }

    if (m_numStreams >= MAX_STREAM_NUM_IN_BUNDLE) {
        ALOGE("%s: stream number (%d) exceeds max limit (%d)",
              __func__, m_numStreams, MAX_STREAM_NUM_IN_BUNDLE);
        return BAD_VALUE;
    }
    QCamera3Stream *pStream = new QCamera3Stream(m_camHandle,
                                               m_handle,
                                               m_camOps,
                                               mPaddingInfo,
                                               this);
    if (pStream == NULL) {
        ALOGE("%s: No mem for Stream", __func__);
        return NO_MEMORY;
    }

    rc = pStream->init(streamType, streamFormat, streamDim, NULL, minStreamBufNum,
                                                    streamCbRoutine, this);
    if (rc == 0) {
        mStreams[m_numStreams] = pStream;
        m_numStreams++;
    } else {
        delete pStream;
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : start
 *
 * DESCRIPTION: start channel, which will start all streams belong to this channel
 *
 * PARAMETERS :
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3Channel::start()
{
    int32_t rc = NO_ERROR;

    if (m_numStreams > 1) {
        ALOGE("%s: bundle not supported", __func__);
    }

    for (uint32_t i = 0; i < m_numStreams; i++) {
        if (mStreams[i] != NULL) {
            mStreams[i]->start();
        }
    }
    rc = m_camOps->start_channel(m_camHandle, m_handle);

    if (rc != NO_ERROR) {
        for (uint32_t i = 0; i < m_numStreams; i++) {
            if (mStreams[i] != NULL) {
                mStreams[i]->stop();
            }
        }
    } else {
        m_bIsActive = true;
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : stop
 *
 * DESCRIPTION: stop a channel, which will stop all streams belong to this channel
 *
 * PARAMETERS : none
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3Channel::stop()
{
    int32_t rc = NO_ERROR;
    if(!m_bIsActive) {
        ALOGE("%s: Attempt to stop inactive channel",__func__);
        return rc;
    }

    for (uint32_t i = 0; i < m_numStreams; i++) {
        if (mStreams[i] != NULL) {
            mStreams[i]->stop();
        }
    }

    m_bIsActive = false;
    return rc;
}

/*===========================================================================
 * FUNCTION   : bufDone
 *
 * DESCRIPTION: return a stream buf back to kernel
 *
 * PARAMETERS :
 *   @recvd_frame  : stream buf frame to be returned
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3Channel::bufDone(mm_camera_super_buf_t *recvd_frame)
{
    int32_t rc = NO_ERROR;
    for (int i = 0; i < recvd_frame->num_bufs; i++) {
         if (recvd_frame->bufs[i] != NULL) {
             for (uint32_t j = 0; j < m_numStreams; j++) {
                 if (mStreams[j] != NULL &&
                     mStreams[j]->getMyHandle() == recvd_frame->bufs[i]->stream_id) {
                     rc = mStreams[j]->bufDone(recvd_frame->bufs[i]->buf_idx);
                     break; // break loop j
                 }
             }
         }
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : getStreamTypeMask
 *
 * DESCRIPTION: Get bit mask of all stream types in this channel
 *
 * PARAMETERS : None
 *
 * RETURN     : Bit mask of all stream types in this channel
 *==========================================================================*/
uint32_t QCamera3Channel::getStreamTypeMask()
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < m_numStreams; i++) {
       mask |= (1U << mStreams[i]->getMyType());
    }
    return mask;
}

/*===========================================================================
 * FUNCTION   : getStreamID
 *
 * DESCRIPTION: Get StreamID of requested stream type
 *
 * PARAMETERS : streamMask
 *
 * RETURN     : Stream ID
 *==========================================================================*/
uint32_t QCamera3Channel::getStreamID(uint32_t streamMask)
{
    uint32_t streamID = 0;
    for (uint32_t i = 0; i < m_numStreams; i++) {
        if (streamMask == (uint32_t )(0x1 << mStreams[i]->getMyType())) {
            streamID = mStreams[i]->getMyServerID();
            break;
        }
    }
    return streamID;
}

/*===========================================================================
 * FUNCTION   : getInternalFormatBuffer
 *
 * DESCRIPTION: return buffer in the internal format structure
 *
 * PARAMETERS :
 *   @streamHandle : buffer handle
 *
 * RETURN     : stream object. NULL if not found
 *==========================================================================*/
mm_camera_buf_def_t* QCamera3RegularChannel::getInternalFormatBuffer(
                                            buffer_handle_t * buffer)
{
    int32_t index;
    if(buffer == NULL)
        return NULL;
    index = mMemory->getMatchBufIndex((void*)buffer);
    if(index < 0) {
        ALOGE("%s: Could not find object among registered buffers",__func__);
        return NULL;
    }
    return mStreams[0]->getInternalFormatBuffer(index);
}

/*===========================================================================
 * FUNCTION   : getStreamByHandle
 *
 * DESCRIPTION: return stream object by stream handle
 *
 * PARAMETERS :
 *   @streamHandle : stream handle
 *
 * RETURN     : stream object. NULL if not found
 *==========================================================================*/
QCamera3Stream *QCamera3Channel::getStreamByHandle(uint32_t streamHandle)
{
    for (uint32_t i = 0; i < m_numStreams; i++) {
        if (mStreams[i] != NULL && mStreams[i]->getMyHandle() == streamHandle) {
            return mStreams[i];
        }
    }
    return NULL;
}

/*===========================================================================
 * FUNCTION   : getStreamByIndex
 *
 * DESCRIPTION: return stream object by index
 *
 * PARAMETERS :
 *   @streamHandle : stream handle
 *
 * RETURN     : stream object. NULL if not found
 *==========================================================================*/
QCamera3Stream *QCamera3Channel::getStreamByIndex(uint32_t index)
{
    if (index < m_numStreams) {
        return mStreams[index];
    }
    return NULL;
}

/*===========================================================================
 * FUNCTION   : streamCbRoutine
 *
 * DESCRIPTION: callback routine for stream
 *
 * PARAMETERS :
 *   @streamHandle : stream handle
 *
 * RETURN     : stream object. NULL if not found
 *==========================================================================*/
void QCamera3Channel::streamCbRoutine(mm_camera_super_buf_t *super_frame,
                QCamera3Stream *stream, void *userdata)
{
    QCamera3Channel *channel = (QCamera3Channel *)userdata;
    if (channel == NULL) {
        ALOGE("%s: invalid channel pointer", __func__);
        return;
    }
    channel->streamCbRoutine(super_frame, stream);
}

/*===========================================================================
 * FUNCTION   : QCamera3RegularChannel
 *
 * DESCRIPTION: constrcutor of QCamera3RegularChannel
 *
 * PARAMETERS :
 *   @cam_handle : camera handle
 *   @cam_ops    : ptr to camera ops table
 *   @cb_routine : callback routine to frame aggregator
 *   @stream     : camera3_stream_t structure
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3RegularChannel::QCamera3RegularChannel(uint32_t cam_handle,
                    mm_camera_ops_t *cam_ops,
                    channel_cb_routine cb_routine,
                    cam_padding_info_t *paddingInfo,
                    void *userData,
                    camera3_stream_t *stream) :
                        QCamera3Channel(cam_handle, cam_ops, cb_routine,
                                                paddingInfo, userData),
                        mCamera3Stream(stream),
                        mNumBufs(0),
                        mCamera3Buffers(NULL),
                        mMemory(NULL),
                        mWidth(stream->width),
                        mHeight(stream->height)
{
}

/*===========================================================================
 * FUNCTION   : QCamera3RegularChannel
 *
 * DESCRIPTION: constrcutor of QCamera3RegularChannel
 *
 * PARAMETERS :
 *   @cam_handle : camera handle
 *   @cam_ops    : ptr to camera ops table
 *   @cb_routine : callback routine to frame aggregator
 *   @stream     : camera3_stream_t structure
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3RegularChannel::QCamera3RegularChannel(uint32_t cam_handle,
                    mm_camera_ops_t *cam_ops,
                    channel_cb_routine cb_routine,
                    cam_padding_info_t *paddingInfo,
                    void *userData,
                    camera3_stream_t *stream,
                    uint32_t width, uint32_t height) :
                        QCamera3Channel(cam_handle, cam_ops, cb_routine,
                                                paddingInfo, userData),
                        mCamera3Stream(stream),
                        mNumBufs(0),
                        mCamera3Buffers(NULL),
                        mMemory(NULL),
                        mWidth(width),
                        mHeight(height)
{
}

/*===========================================================================
 * FUNCTION   : ~QCamera3RegularChannel
 *
 * DESCRIPTION: destructor of QCamera3RegularChannel
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3RegularChannel::~QCamera3RegularChannel()
{
    if (mCamera3Buffers) {
        delete[] mCamera3Buffers;
    }

    streamDim.width = (int32_t)mCamera3Stream->width;
    streamDim.height = (int32_t)mCamera3Stream->height;

    rc = QCamera3Channel::addStream(mStreamType,
            streamFormat,
            streamDim,
            mNumBufs,
            mPostProcMask);

    return rc;

}

int32_t QCamera3RegularChannel::initialize()
{
  //TO DO
  return 0;
}

/*===========================================================================
 * FUNCTION   : request
 *
 * DESCRIPTION: process a request from camera service. Stream on if ncessary.
 *
 * PARAMETERS :
 *   @buffer  : buffer to be filled for this request
 *
 * RETURN     : 0 on a success start of capture
 *              -EINVAL on invalid input
 *              -ENODEV on serious error
 *==========================================================================*/
int32_t QCamera3RegularChannel::request(buffer_handle_t *buffer, uint32_t frameNumber)
{
    //FIX ME: Return buffer back in case of failures below.

    int32_t rc = NO_ERROR;
    int index;
    if(!m_bIsActive) {
        ALOGD("%s: First request on this channel starting stream",__func__);
        start();
        if(rc != NO_ERROR) {
            ALOGE("%s: Failed to start the stream on the request",__func__);
            return rc;
        }
    } else {
        ALOGV("%s: Request on an existing stream",__func__);
    }

    if(!mMemory) {
        ALOGE("%s: error, Gralloc Memory object not yet created for this stream",__func__);
        return NO_MEMORY;
    }

    index = mMemory->getMatchBufIndex((void*)buffer);
    if(index < 0) {
        ALOGE("%s: Could not find object among registered buffers",__func__);
        return DEAD_OBJECT;
    }

    rc = mStreams[0]->bufDone((uint32_t)index);
    if(rc != NO_ERROR) {
        ALOGE("%s: Failed to Q new buffer to stream",__func__);
        return rc;
    }

    rc = mMemory.markFrameNumber((uint32_t)index, frameNumber);
    return rc;
}

/*===========================================================================
 * FUNCTION   : registerBuffers
 *
 * DESCRIPTION: register streaming buffers to the channel object
 *
 * PARAMETERS :
 *   @num_buffers : number of buffers to be registered
 *   @buffers     : buffer to be registered
 *
 * RETURN     : 0 on a success start of capture
 *              -EINVAL on invalid input
 *              -ENOMEM on failure to register the buffer
 *              -ENODEV on serious error
 *==========================================================================*/
int32_t QCamera3RegularChannel::registerBuffers(uint32_t num_buffers, buffer_handle_t **buffers)
{
    int rc = 0;
    struct private_handle_t *priv_handle = (struct private_handle_t *)(*buffers[0]);
    cam_stream_type_t streamType;
    cam_format_t streamFormat;
    cam_dimension_t streamDim;

    rc = init(NULL, NULL);
    if (rc < 0) {
        ALOGE("%s: init failed", __func__);
        return rc;
    }

    if (mCamera3Stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        if (priv_handle->flags & private_handle_t::PRIV_FLAGS_VIDEO_ENCODER) {
            streamType = CAM_STREAM_TYPE_VIDEO;
            streamFormat = CAM_FORMAT_YUV_420_NV12;
        } else if (priv_handle->flags & private_handle_t::PRIV_FLAGS_HW_TEXTURE) {
            streamType = CAM_STREAM_TYPE_PREVIEW;
            streamFormat = CAM_FORMAT_YUV_420_NV21;
        } else {
            //TODO: Add a new flag in libgralloc for ZSL buffers, and its size needs
            // to be properly aligned and padded.
            ALOGE("%s: priv_handle->flags 0x%x not supported",
                    __func__, priv_handle->flags);
            streamType = CAM_STREAM_TYPE_SNAPSHOT;
            streamFormat = CAM_FORMAT_YUV_420_NV21;
        }
    } else if(mCamera3Stream->format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
         streamType = CAM_STREAM_TYPE_CALLBACK;
         streamFormat = CAM_FORMAT_YUV_420_NV21;
    } else {
        //TODO: Fail for other types of streams for now
        ALOGE("%s: format is not IMPLEMENTATION_DEFINED or flexible", __func__);
        return -EINVAL;
    }

    /* Bookkeep buffer set because they go out of scope after register call */
    mNumBufs = num_buffers;
    mCamera3Buffers = new buffer_handle_t*[num_buffers];
    if (mCamera3Buffers == NULL) {
        ALOGE("%s: Failed to allocate buffer_handle_t*", __func__);
        return -ENOMEM;
    }
    for (size_t i = 0; i < num_buffers; i++)
        mCamera3Buffers[i] = buffers[i];

    streamDim.width = mWidth;
    streamDim.height = mHeight;

    rc = QCamera3Channel::addStream(streamType, streamFormat, streamDim,
        num_buffers);
    return rc;
}

void QCamera3RegularChannel::streamCbRoutine(
                            mm_camera_super_buf_t *super_frame,
                            QCamera3Stream *stream)
{
    //FIXME Q Buf back in case of error?
    uint8_t frameIndex;
    buffer_handle_t *resultBuffer;
    int32_t resultFrameNumber;
    camera3_stream_buffer_t result;

    if(!super_frame) {
         ALOGE("%s: Invalid Super buffer",__func__);
         return;
    }

    if(super_frame->num_bufs != 1) {
         ALOGE("%s: Multiple streams are not supported",__func__);
         return;
    }
    if(super_frame->bufs[0] == NULL ) {
         ALOGE("%s: Error, Super buffer frame does not contain valid buffer",
                  __func__);
         return;
    }

    frameIndex = (uint8_t)super_frame->bufs[0]->buf_idx;
    if(frameIndex >= mNumBufs) {
         ALOGE("%s: Error, Invalid index for buffer",__func__);
         if(stream) {
             stream->bufDone(frameIndex);
         }
         return;
    }

    ////Use below data to issue framework callback
    resultBuffer = mCamera3Buffers[frameIndex];
    resultFrameNumber = mMemory->getFrameNumber(frameIndex);

    result.stream = mCamera3Stream;
    result.buffer = resultBuffer;
    result.status = CAMERA3_BUFFER_STATUS_OK;
    result.acquire_fence = -1;
    result.release_fence = -1;

    if (0 <= resultFrameNumber) {
        mChannelCB(NULL, &result, (uint32_t)resultFrameNumber, mUserData);
    } else {
        ALOGE("%s: Bad brame number", __func__);
    }
    free(super_frame);
    return;
}

QCamera3Memory* QCamera3RegularChannel::getStreamBufs(uint32_t /*len*/)
{
    if (mNumBufs == 0 || mCamera3Buffers == NULL) {
        ALOGE("%s: buffers not registered yet", __func__);
        return NULL;
    }

    mMemory = new QCamera3GrallocMemory();
    if (mMemory == NULL) {
        return NULL;
    }

    if (mMemory->registerBuffers(mNumBufs, mCamera3Buffers) < 0) {
        delete mMemory;
        mMemory = NULL;
        return NULL;
    }
    return mMemory;
}

void QCamera3RegularChannel::putStreamBufs()
{
    mMemory->unregisterBuffers();
    delete mMemory;
    mMemory = NULL;
}

uint32_t QCamera3RegularChannel::kMaxBuffers = MAX_INFLIGHT_REQUESTS;

QCamera3MetadataChannel::QCamera3MetadataChannel(uint32_t cam_handle,
                    mm_camera_ops_t *cam_ops,
                    channel_cb_routine cb_routine,
                    cam_padding_info_t *paddingInfo,
                    void *userData) :
                        QCamera3Channel(cam_handle, cam_ops,
                                cb_routine, paddingInfo, userData),
                        mMemory(NULL)
{
}

QCamera3MetadataChannel::~QCamera3MetadataChannel()
{
    if (m_bIsActive)
        stop();

    if (mMemory) {
        mMemory->deallocate();
        delete mMemory;
        mMemory = NULL;
    }
}

int32_t QCamera3MetadataChannel::initialize()
{
    int32_t rc;
    cam_dimension_t streamDim;

    if (mMemory || m_numStreams > 0) {
        ALOGE("%s: metadata channel already initialized", __func__);
        return -EINVAL;
    }

    rc = init(NULL, NULL);
    if (rc < 0) {
        ALOGE("%s: init failed", __func__);
        return rc;
    }

    streamDim.width = (int32_t)sizeof(metadata_buffer_t),
    streamDim.height = 1;
    rc = QCamera3Channel::addStream(CAM_STREAM_TYPE_METADATA, CAM_FORMAT_MAX,
        streamDim, MIN_STREAMING_BUFFER_NUM);
    if (rc < 0) {
        ALOGE("%s: addStream failed", __func__);
    }
    return rc;
}

int32_t QCamera3MetadataChannel::request(buffer_handle_t * /*buffer*/,
                                                uint32_t /*frameNumber*/)
{
    if (!m_bIsActive) {
        return start();
    }
    else
        return 0;
}

int32_t QCamera3MetadataChannel::registerBuffers(uint32_t /*num_buffers*/,
                                        buffer_handle_t ** /*buffers*/)
{
    // no registerBuffers are supported for metadata channel
    return -EINVAL;
}

void QCamera3MetadataChannel::streamCbRoutine(
                        mm_camera_super_buf_t *super_frame,
                        QCamera3Stream * /*stream*/)
{
    uint32_t requestNumber = 0;
    if (super_frame == NULL || super_frame->num_bufs != 1) {
        ALOGE("%s: super_frame is not valid", __func__);
        return;
    }
    mChannelCB(super_frame, NULL, requestNumber, mUserData);
}

QCamera3Memory* QCamera3MetadataChannel::getStreamBufs(uint32_t len)
{
    int rc;
    if (len < sizeof(metadata_buffer_t)) {
        ALOGE("%s: size doesn't match %d vs %d", __func__,
                len, sizeof(metadata_buffer_t));
        return NULL;
    }
    mMemory = new QCamera3HeapMemory();
    if (!mMemory) {
        ALOGE("%s: unable to create metadata memory", __func__);
        return NULL;
    }
    rc = mMemory->allocate(MIN_STREAMING_BUFFER_NUM, len, true);
    if (rc < 0) {
        ALOGE("%s: unable to allocate metadata memory", __func__);
        delete mMemory;
        mMemory = NULL;
        return NULL;
    }
    memset(mMemory->getPtr(0), 0, sizeof(metadata_buffer_t));
    return mMemory;
}

void QCamera3MetadataChannel::putStreamBufs()
{
    mMemory->deallocate();
    delete mMemory;
    mMemory = NULL;
}

/*************************************************************************************/
// RAW Channel related functions
uint32_t QCamera3RawChannel::kMaxBuffers = MAX_INFLIGHT_REQUESTS;

QCamera3RawChannel::QCamera3RawChannel(uint32_t cam_handle,
                    mm_camera_ops_t *cam_ops,
                    channel_cb_routine cb_routine,
                    cam_padding_info_t *paddingInfo,
                    void *userData,
                    camera3_stream_t *stream,
                    uint32_t postprocess_mask,
                    bool raw_16) :
                        QCamera3RegularChannel(cam_handle, cam_ops,
                                cb_routine, paddingInfo, userData, stream,
                                CAM_STREAM_TYPE_RAW, postprocess_mask),
                        mIsRaw16(raw_16)
{
    char prop[PROPERTY_VALUE_MAX];
    property_get("persist.camera.raw.debug.dump", prop, "0");
    mRawDump = atoi(prop);
}

QCamera3RawChannel::~QCamera3RawChannel()
{
}

void QCamera3RawChannel::streamCbRoutine(
                        mm_camera_super_buf_t *super_frame,
                        QCamera3Stream * stream)
{
    ATRACE_CALL();
    /* Move this back down once verified */
    if (mRawDump)
        dumpRawSnapshot(super_frame->bufs[0]);

    if (mIsRaw16) {
        if (RAW_FORMAT == CAM_FORMAT_BAYER_MIPI_RAW_10BPP_GBRG)
            convertMipiToRaw16(super_frame->bufs[0]);
        else
            convertLegacyToRaw16(super_frame->bufs[0]);
    }

    //Make sure cache coherence because extra processing is done
    mMemory.cleanInvalidateCache(super_frame->bufs[0]->buf_idx);

    QCamera3RegularChannel::streamCbRoutine(super_frame, stream);
    return;
}

void QCamera3RawChannel::dumpRawSnapshot(mm_camera_buf_def_t *frame)
{
   QCamera3Stream *stream = getStreamByIndex(0);
   if (stream != NULL) {
       char buf[32];
       memset(buf, 0, sizeof(buf));
       cam_dimension_t dim;
       memset(&dim, 0, sizeof(dim));
       stream->getFrameDimension(dim);

       cam_frame_len_offset_t offset;
       memset(&offset, 0, sizeof(cam_frame_len_offset_t));
       stream->getFrameOffset(offset);
       snprintf(buf, sizeof(buf), "/data/local/tmp/r_%d_%dx%d.raw",
                frame->frame_idx, offset.mp[0].stride, offset.mp[0].scanline);

       int file_fd = open(buf, O_RDWR| O_CREAT, 0644);
       if (file_fd >= 0) {
          ssize_t written_len = write(file_fd, frame->buffer, frame->frame_len);
          ALOGE("%s: written number of bytes %zd", __func__, written_len);
          close(file_fd);
       } else {
          ALOGE("%s: failed to open file to dump image", __func__);
       }
   } else {
       ALOGE("%s: Could not find stream", __func__);
   }

}

void QCamera3RawChannel::convertLegacyToRaw16(mm_camera_buf_def_t *frame)
{
    // Convert image buffer from Opaque raw format to RAW16 format
    // 10bit Opaque raw is stored in the format of:
    // 0000 - p5 - p4 - p3 - p2 - p1 - p0
    // where p0 to p5 are 6 pixels (each is 10bit)_and most significant
    // 4 bits are 0s. Each 64bit word contains 6 pixels.

    QCamera3Stream *stream = getStreamByIndex(0);
    cam_dimension_t dim;
    memset(&dim, 0, sizeof(dim));
    stream->getFrameDimension(dim);

    cam_frame_len_offset_t offset;
    memset(&offset, 0, sizeof(cam_frame_len_offset_t));
    stream->getFrameOffset(offset);

    uint32_t raw16_stride = ((uint32_t)dim.width + 15U) & ~15U;
    uint16_t* raw16_buffer = (uint16_t *)frame->buffer;

    // In-place format conversion.
    // Raw16 format always occupy more memory than opaque raw10.
    // Convert to Raw16 by iterating through all pixels from bottom-right
    // to top-left of the image.
    // One special notes:
    // 1. Cross-platform raw16's stride is 16 pixels.
    // 2. Opaque raw10's stride is 6 pixels, and aligned to 16 bytes.
    for (int32_t ys = dim.height - 1; ys >= 0; ys--) {
        uint32_t y = (uint32_t)ys;
        uint64_t* row_start = (uint64_t *)frame->buffer +
            y * (uint32_t)offset.mp[0].stride / 6;
        for (int32_t xs = dim.width - 1; xs >= 0; xs--) {
            uint32_t x = (uint32_t)xs;
            uint16_t raw16_pixel = 0x3FF & (row_start[x/6] >> (10*(x%6)));
            raw16_buffer[y*raw16_stride+x] = raw16_pixel;
        }
    }
}

void QCamera3RawChannel::convertMipiToRaw16(mm_camera_buf_def_t *frame)
{
    // Convert image buffer from mipi10 raw format to RAW16 format
    // mipi10 opaque raw is stored in the format of:
    // P3(1:0) P2(1:0) P1(1:0) P0(1:0) P3(9:2) P2(9:2) P1(9:2) P0(9:2)
    // 4 pixels occupy 5 bytes, no padding needed

    QCamera3Stream *stream = getStreamByIndex(0);
    cam_dimension_t dim;
    memset(&dim, 0, sizeof(dim));
    stream->getFrameDimension(dim);

    cam_frame_len_offset_t offset;
    memset(&offset, 0, sizeof(cam_frame_len_offset_t));
    stream->getFrameOffset(offset);

    uint32_t raw16_stride = ((uint32_t)dim.width + 15U) & ~15U;
    uint16_t* raw16_buffer = (uint16_t *)frame->buffer;

    // In-place format conversion.
    // Raw16 format always occupy more memory than opaque raw10.
    // Convert to Raw16 by iterating through all pixels from bottom-right
    // to top-left of the image.
    // One special notes:
    // 1. Cross-platform raw16's stride is 16 pixels.
    // 2. mipi raw10's stride is 4 pixels, and aligned to 16 bytes.
    for (int32_t ys = dim.height - 1; ys >= 0; ys--) {
        uint32_t y = (uint32_t)ys;
        uint8_t* row_start = (uint8_t *)frame->buffer +
            y * (uint32_t)offset.mp[0].stride;
        for (int32_t xs = dim.width - 1; xs >= 0; xs--) {
            uint32_t x = (uint32_t)xs;
            uint8_t upper_8bit = row_start[5*(x/4)+x%4];
            uint8_t lower_2bit = ((row_start[5*(x/4)+4] >> (x%4)) & 0x3);
            uint16_t raw16_pixel = (uint16_t)(((uint16_t)upper_8bit)<<2 | (uint16_t)lower_2bit);
            raw16_buffer[y*raw16_stride+x] = raw16_pixel;
        }
    }

}


/*************************************************************************************/
// RAW Dump Channel related functions

uint32_t QCamera3RawDumpChannel::kMaxBuffers = 3U;
/*===========================================================================
 * FUNCTION   : QCamera3RawDumpChannel
 *
 * DESCRIPTION: Constructor for RawDumpChannel
 *
 * PARAMETERS :
 *   @cam_handle    : Handle for Camera
 *   @cam_ops       : Function pointer table
 *   @rawDumpSize   : Dimensions for the Raw stream
 *   @paddinginfo   : Padding information for stream
 *   @userData      : Cookie for parent
 *   @pp mask       : PP feature mask for this stream
 *
 * RETURN           : NA
 *==========================================================================*/
QCamera3RawDumpChannel::QCamera3RawDumpChannel(uint32_t cam_handle,
                    mm_camera_ops_t *cam_ops,
                    cam_dimension_t rawDumpSize,
                    cam_padding_info_t *paddingInfo,
                    void *userData,
                    uint32_t postprocess_mask) :
                        QCamera3Channel(cam_handle, cam_ops, NULL,
                                paddingInfo, postprocess_mask, userData),
                        mDim(rawDumpSize),
                        mMemory(NULL)
{
    char prop[PROPERTY_VALUE_MAX];
    property_get("persist.camera.raw.dump", prop, "0");
    mRawDump = atoi(prop);
}

/*===========================================================================
 * FUNCTION   : QCamera3RawDumpChannel
 *
 * DESCRIPTION: Destructor for RawDumpChannel
 *
 * PARAMETERS :
 *
 * RETURN           : NA
 *==========================================================================*/

QCamera3RawDumpChannel::~QCamera3RawDumpChannel()
{
}

/*===========================================================================
 * FUNCTION   : dumpRawSnapshot
 *
 * DESCRIPTION: Helper function to dump Raw frames
 *
 * PARAMETERS :
 *  @frame      : stream buf frame to be dumped
 *
 *  RETURN      : NA
 *==========================================================================*/
void QCamera3RawDumpChannel::dumpRawSnapshot(mm_camera_buf_def_t *frame)
{
    QCamera3Stream *stream = getStreamByIndex(0);
    char buf[128];
    struct timeval tv;
    struct tm *timeinfo;

    cam_dimension_t dim;
    memset(&dim, 0, sizeof(dim));
    stream->getFrameDimension(dim);

    cam_frame_len_offset_t offset;
    memset(&offset, 0, sizeof(cam_frame_len_offset_t));
    stream->getFrameOffset(offset);

    gettimeofday(&tv, NULL);
    timeinfo = localtime(&tv.tv_sec);

    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf),
                 "/data/%04d-%02d-%02d-%02d-%02d-%02d-%06ld_%d_%dx%d.raw",
                 timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
                 timeinfo->tm_mday, timeinfo->tm_hour,
                 timeinfo->tm_min, timeinfo->tm_sec,tv.tv_usec,
                 frame->frame_idx, dim.width, dim.height);

    int file_fd = open(buf, O_RDWR| O_CREAT, 0777);
    if (file_fd >= 0) {
        ssize_t written_len = write(file_fd, frame->buffer, offset.frame_len);
        CDBG("%s: written number of bytes %zd", __func__, written_len);
        close(file_fd);
    } else {
        ALOGE("%s: failed to open file to dump image", __func__);
    }
}

/*===========================================================================
 * FUNCTION   : streamCbRoutine
 *
 * DESCRIPTION: Callback routine invoked for each frame generated for
 *              Rawdump channel
 *
 * PARAMETERS :
 *   @super_frame  : stream buf frame generated
 *   @stream       : Underlying Stream object cookie
 *
 * RETURN          : NA
 *==========================================================================*/
void QCamera3RawDumpChannel::streamCbRoutine(mm_camera_super_buf_t *super_frame,
                                                QCamera3Stream *stream)
{
    CDBG("%s: E",__func__);
    if (super_frame == NULL || super_frame->num_bufs != 1) {
        ALOGE("%s: super_frame is not valid", __func__);
        return;
    }

    if (mRawDump)
        dumpRawSnapshot(super_frame->bufs[0]);

    bufDone(super_frame);
    free(super_frame);
}

/*===========================================================================
 * FUNCTION   : getStreamBufs
 *
 * DESCRIPTION: Callback function provided to interface to get buffers.
 *
 * PARAMETERS :
 *   @len       : Length of each buffer to be allocated
 *
 * RETURN     : NULL on buffer allocation failure
 *              QCamera3Memory object on sucess
 *==========================================================================*/
QCamera3Memory* QCamera3RawDumpChannel::getStreamBufs(uint32_t len)
{
    int rc;
    mMemory = new QCamera3HeapMemory();

    if (!mMemory) {
        ALOGE("%s: unable to create heap memory", __func__);
        return NULL;
    }
    rc = mMemory->allocate(kMaxBuffers, (size_t)len, true);
    if (rc < 0) {
        ALOGE("%s: unable to allocate heap memory", __func__);
        delete mMemory;
        mMemory = NULL;
        return NULL;
    }
    return mMemory;
}

/*===========================================================================
 * FUNCTION   : putStreamBufs
 *
 * DESCRIPTION: Callback function provided to interface to return buffers.
 *              Although no handles are actually returned, implicitl assumption
 *              that interface will no longer use buffers and channel can
 *              deallocated if necessary.
 *
 * PARAMETERS : NA
 *
 * RETURN     : NA
 *==========================================================================*/
void QCamera3RawDumpChannel::putStreamBufs()
{
    mMemory->deallocate();
    delete mMemory;
    mMemory = NULL;
}

/*===========================================================================
 * FUNCTION : request
 *
 * DESCRIPTION: Request function used as trigger
 *
 * PARAMETERS :
 * @recvd_frame : buffer- this will be NULL since this is internal channel
 * @frameNumber : Undefined again since this is internal stream
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3RawDumpChannel::request(buffer_handle_t * /*buffer*/,
                                                uint32_t /*frameNumber*/)
{
    if (!m_bIsActive) {
        return QCamera3Channel::start();
    }
    else
        return 0;
}

/*===========================================================================
 * FUNCTION : intialize
 *
 * DESCRIPTION: Initializes channel params and creates underlying stream
 *
 * PARAMETERS : NA
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3RawDumpChannel::initialize()
{
    int32_t rc;

    rc = init(NULL, NULL);
    if (rc < 0) {
        ALOGE("%s: init failed", __func__);
        return rc;
    }

    rc = QCamera3Channel::addStream(CAM_STREAM_TYPE_RAW, CAM_FORMAT_BAYER_MIPI_RAW_10BPP_GBRG,
            mDim, (uint8_t)kMaxBuffers, mPostProcMask);
    if (rc < 0) {
        ALOGE("%s: addStream failed", __func__);
    }
    return rc;
}
/*************************************************************************************/

/*===========================================================================
 * FUNCTION   : jpegEvtHandle
 *
 * DESCRIPTION: Function registerd to mm-jpeg-interface to handle jpeg events.
                Construct result payload and call mChannelCb to deliver buffer
                to framework.
 *
 * PARAMETERS :
 *   @status    : status of jpeg job
 *   @client_hdl: jpeg client handle
 *   @jobId     : jpeg job Id
 *   @p_ouput   : ptr to jpeg output result struct
 *   @userdata  : user data ptr
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3PicChannel::jpegEvtHandle(jpeg_job_status_t status,
                                              uint32_t /*client_hdl*/,
                                              uint32_t jobId,
                                              mm_jpeg_output_t *p_output,
                                              void *userdata)
{

    ATRACE_CALL();
    buffer_handle_t *resultBuffer, *jpegBufferHandle;
    int resultStatus = CAMERA3_BUFFER_STATUS_OK;
    camera3_stream_buffer_t result;
    camera3_jpeg_blob_t jpegHeader;

    QCamera3PicChannel *obj = (QCamera3PicChannel *)userdata;
    if (obj) {

        //Release any cached metabuffer information
        if (obj->mMetaFrame != NULL && obj->m_pMetaChannel != NULL) {
            ((QCamera3MetadataChannel*)(obj->m_pMetaChannel))->bufDone(obj->mMetaFrame);
            obj->mMetaFrame = NULL;
            obj->m_pMetaChannel = NULL;
        } else {
            ALOGE("%s: Meta frame was NULL", __func__);
        }
        //Construct payload for process_capture_result. Call mChannelCb

        qcamera_jpeg_data_t *job = obj->m_postprocessor.findJpegJobByJobId(jobId);

        if ((job == NULL) || (status == JPEG_JOB_STATUS_ERROR)) {
            ALOGE("%s: Error in jobId: (%d) with status: %d", __func__, jobId, status);
            resultStatus = CAMERA3_BUFFER_STATUS_ERROR;
        }

        uint32_t bufIdx = (uint32_t)job->jpeg_settings->out_buf_index;
        CDBG("%s: jpeg out_buf_index: %d", __func__, bufIdx);

        //Construct jpeg transient header of type camera3_jpeg_blob_t
        //Append at the end of jpeg image of buf_filled_len size

        jpegHeader.jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
        jpegHeader.jpeg_size = (uint32_t)p_output->buf_filled_len;


        char* jpeg_buf = (char *)p_output->buf_vaddr;
        ssize_t maxJpegSize = -1;

        if(obj->mJpegSettings->max_jpeg_size <= 0 ||
                obj->mJpegSettings->max_jpeg_size > obj->mMemory->getSize(obj->mCurrentBufIndex)){
            ALOGW("%s:Max Jpeg size :%d is out of valid range setting to size of buffer",
                    __func__, obj->mJpegSettings->max_jpeg_size);
            maxJpegSize =  obj->mMemory->getSize(obj->mCurrentBufIndex);
        } else {
            maxJpegSize = obj->mJpegSettings->max_jpeg_size;
            ALOGI("%s: Setting max jpeg size to %d",__func__, maxJpegSize);
        }

        size_t jpeg_eof_offset = (size_t)(maxJpegSize - (ssize_t)sizeof(jpegHeader));
        char *jpeg_eof = &jpeg_buf[jpeg_eof_offset];
        memcpy(jpeg_eof, &jpegHeader, sizeof(jpegHeader));
        obj->mMemory->cleanInvalidateCache(obj->mCurrentBufIndex);

        ////Use below data to issue framework callback
        resultBuffer = (buffer_handle_t *)obj->mMemory.getBufferHandle(bufIdx);
        int32_t resultFrameNumber = obj->mMemory.getFrameNumber(bufIdx);

        result.stream = obj->mCamera3Stream;
        result.buffer = resultBuffer;
        result.status = resultStatus;
        result.acquire_fence = -1;
        result.release_fence = -1;

        // Release any snapshot buffers before calling
        // the user callback. The callback can potentially
        // unblock pending requests to snapshot stream.
        if (NULL != job) {
            int32_t snapshotIdx = -1;
            mm_camera_super_buf_t* src_frame = NULL;

            if (job->src_reproc_frame)
                src_frame = job->src_reproc_frame;
            else
                src_frame = job->src_frame;

            if (src_frame) {
                if (obj->mStreams[0]->getMyHandle() ==
                        src_frame->bufs[0]->stream_id) {
                    snapshotIdx = (int32_t)src_frame->bufs[0]->buf_idx;
                } else {
                    ALOGE("%s: Snapshot stream id %d and source frame %d don't match!",
                            __func__, obj->mStreams[0]->getMyHandle(),
                            src_frame->bufs[0]->stream_id);
                }
            }
            if (0 <= snapshotIdx) {
                Mutex::Autolock lock(obj->mFreeBuffersLock);
                obj->mFreeBufferList.push_back((uint32_t)snapshotIdx);
            } else {
                ALOGE("%s: Snapshot buffer not found!", __func__);
            }
        }

        CDBG("%s: Issue Callback", __func__);
        obj->mChannelCB(NULL, &result, (uint32_t)resultFrameNumber, obj->mUserData);

        // release internal data for jpeg job
        if (job != NULL) {
            obj->m_postprocessor.releaseJpegJobData(job);
            free(job);
        }
        return;
        // }
    } else {
        ALOGE("%s: Null userdata in jpeg callback", __func__);
    }
}

QCamera3PicChannel::QCamera3PicChannel(uint32_t cam_handle,
                    mm_camera_ops_t *cam_ops,
                    channel_cb_routine cb_routine,
                    cam_padding_info_t *paddingInfo,
                    void *userData,
                    camera3_stream_t *stream) :
                        QCamera3Channel(cam_handle, cam_ops, cb_routine,
                        paddingInfo, userData),
                        m_postprocessor(this),
                        mCamera3Stream(stream),
                        mNumBufsRegistered(CAM_MAX_NUM_BUFS_PER_STREAM),
                        mNumSnapshotBufs(0),
                        mCurrentBufIndex(0U),
                        mPostProcStarted(false),
                        mInputBufferConfig(false),
                        mYuvMemory(NULL),
                        mMetaFrame(NULL)
{
    int32_t rc = m_postprocessor.init(jpegEvtHandle, this);
    if (rc != 0) {
        ALOGE("Init Postprocessor failed");
    }
}

QCamera3PicChannel::~QCamera3PicChannel()
{
    int32_t rc = m_postprocessor.deinit();
    if (rc != 0) {
        ALOGE("De-init Postprocessor failed");
    }
    if (mCamera3Buffers) {
        delete[] mCamera3Buffers;
    }
}

int32_t QCamera3PicChannel::initialize()
{
    int32_t rc = NO_ERROR;
    cam_dimension_t streamDim;
    cam_stream_type_t streamType;
    cam_format_t streamFormat;
    mm_camera_channel_attr_t attr;

    memset(&attr, 0, sizeof(mm_camera_channel_attr_t));
    attr.notify_mode = MM_CAMERA_SUPER_BUF_NOTIFY_BURST;
    attr.look_back = 1;
    attr.post_frame_skip = 1;
    attr.water_mark = 1;
    attr.max_unmatched_frames = 1;

    rc = init(&attr, NULL);
    if (rc < 0) {
        ALOGE("%s: init failed", __func__);
        return rc;
    }

    streamType = mStreamType;
    streamFormat = mStreamFormat;
    streamDim.width = (int32_t)mYuvWidth;
    streamDim.height = (int32_t)mYuvHeight;

    rc = QCamera3Channel::addStream(streamType, streamFormat, streamDim,
            num_buffers);

    return rc;
}

int32_t QCamera3PicChannel::request(buffer_handle_t *buffer,
        uint32_t frameNumber, jpeg_settings_t* jpegSettings,
        mm_camera_buf_def_t *pInputBuffer,QCamera3Channel* pInputChannel)
{
    //FIX ME: Return buffer back in case of failures below.

    int32_t rc = NO_ERROR;

    reprocess_config_t reproc_cfg;
    memset(&reproc_cfg, 0, sizeof(reprocess_config_t));
    reproc_cfg.padding = mPaddingInfo;
    if (NULL != pInputBuffer) {
        reproc_cfg.input_stream_dim.width = (int32_t)pInputBuffer->stream->width;
        reproc_cfg.input_stream_dim.height = (int32_t)pInputBuffer->stream->height;
    } else {
        reproc_cfg.input_stream_dim.width = (int32_t)mYuvWidth;
        reproc_cfg.input_stream_dim.height = (int32_t)mYuvHeight;
        reproc_cfg.src_channel = this;
    }
    reproc_cfg.output_stream_dim.width = (int32_t)mCamera3Stream->width;
    reproc_cfg.output_stream_dim.height = (int32_t)mCamera3Stream->height;
    reproc_cfg.stream_type = mStreamType;
    reproc_cfg.stream_format = mStreamFormat;
    rc = mm_stream_calc_offset_snapshot(mStreamFormat, &reproc_cfg.input_stream_dim,
            reproc_cfg.padding, &reproc_cfg.input_stream_plane_info);
    if (rc != 0) {
        ALOGE("%s: Snapshot stream plane info calculation failed!", __func__);
        return rc;
    }

    // Picture stream has already been started before any request comes in
    if (!m_bIsActive) {
        ALOGE("%s: Channel not started!!", __func__);
        return NO_INIT;
    }

    int index = mMemory.getMatchBufIndex((void*)buffer);

    if(index < 0) {
        rc = registerBuffer(buffer);
        if (NO_ERROR != rc) {
            ALOGE("%s: On-the-fly buffer registration failed %d",
                    __func__, rc);
            return rc;
        }

        index = mMemory.getMatchBufIndex((void*)buffer);
        if (index < 0) {
            ALOGE("%s: Could not find object among registered buffers",__func__);
            return DEAD_OBJECT;
        }
    }
    CDBG("%s: buffer index %d, frameNumber: %u", __func__, index, frameNumber);

    rc = mMemory.markFrameNumber((uint32_t)index, frameNumber);

    //Start the postprocessor for jpeg encoding. Pass mMemory as destination buffer
    mCurrentBufIndex = (uint32_t)index;

    // Start postprocessor
    // This component needs to be re-configured
    // once we switch from input(framework) buffer
    // reprocess to standard capture!
    bool restartNeeded = ((!mInputBufferConfig) != (NULL != pInputBuffer));
    if((!mPostProcStarted) || restartNeeded) {
        m_postprocessor.start(reproc_cfg, metadata);
        mPostProcStarted = true;
        mInputBufferConfig = (NULL == pInputBuffer);
    }

    // Queue jpeg settings
    rc = queueJpegSetting((uint32_t)index, metadata);

    if (pInputBuffer == NULL) {
        Mutex::Autolock lock(mFreeBuffersLock);
        if (!mFreeBufferList.empty()) {
            List<uint32_t>::iterator it = mFreeBufferList.begin();
            uint32_t freeBuffer = *it;
            mStreams[0]->bufDone(freeBuffer);
            mFreeBufferList.erase(it);
        } else {
            ALOGE("%s: No snapshot buffers available!", __func__);
            rc = NOT_ENOUGH_DATA;
        }
    } else {
        if (0 < mOfflineMetaMemory.getCnt()) {
            mOfflineMetaMemory.deallocate();
        }
        if (0 < mOfflineMemory.getCnt()) {
            mOfflineMemory.unregisterBuffers();
        }

        int input_index = mOfflineMemory.getMatchBufIndex((void*)pInputBuffer->buffer);
        if(input_index < 0) {
            rc = mOfflineMemory.registerBuffer(pInputBuffer->buffer);
            if (NO_ERROR != rc) {
                ALOGE("%s: On-the-fly input buffer registration failed %d",
                        __func__, rc);
                return rc;

            }

            //Registering Jpeg output buffer
            if (mMemory->registerBuffers(mNumBufs, mCamera3Buffers) < 0) {
                delete mMemory;
                mMemory = NULL;
                return NO_MEMORY;
            }
        } else {
            ALOGE("%s: error, Gralloc Memory object not yet created for this stream",__func__);
            return NO_MEMORY;
        }

        memset(src_frame, 0, sizeof(qcamera_fwk_input_pp_data_t));
        src_frame->src_frame = *pInputBuffer;
        rc = mOfflineMemory.getBufDef(reproc_cfg.input_stream_plane_info.plane_info,
                src_frame->input_buffer, (uint32_t)input_index);
        if (rc != 0) {
            free(src_frame);
            return rc;
        }
        if (mYUVDump) {
            dumpYUV(&src_frame->input_buffer, reproc_cfg.input_stream_dim,
                    reproc_cfg.input_stream_plane_info.plane_info, 1);
        }
        cam_dimension_t dim = {(int)sizeof(metadata_buffer_t), 1};
        cam_stream_buf_plane_info_t meta_planes;
        rc = mm_stream_calc_offset_metadata(&dim, mPaddingInfo, &meta_planes);
        if (rc != 0) {
            ALOGE("%s: Metadata stream plane info calculation failed!", __func__);
            free(src_frame);
            return rc;
        }

        rc = mOfflineMetaMemory.allocate(1, sizeof(metadata_buffer_t), false);
        if (NO_ERROR != rc) {
            ALOGE("%s: Couldn't allocate offline metadata buffer!", __func__);
            free(src_frame);
            return rc;
        }
        mm_camera_buf_def_t meta_buf;
        cam_frame_len_offset_t offset = meta_planes.plane_info;
        rc = mOfflineMetaMemory.getBufDef(offset, meta_buf, 0);
        if (NO_ERROR != rc) {
            free(src_frame);
            return rc;
        }
        memcpy(meta_buf.buffer, metadata, sizeof(metadata_buffer_t));
        src_frame->metadata_buffer = meta_buf;
        src_frame->reproc_config = reproc_cfg;

        CDBG_HIGH("%s: Post-process started", __func__);
        CDBG_HIGH("%s: Issue call to reprocess", __func__);

        m_postprocessor.processData(src_frame);

    }

    index = mMemory->getMatchBufIndex((void*)buffer);
    if(index < 0) {
        ALOGE("%s: Could not find object among registered buffers",__func__);
        return DEAD_OBJECT;
    }
    rc = mMemory->markFrameNumber(index, frameNumber);

    //Start the postprocessor for jpeg encoding. Pass mMemory as destination buffer
    mCurrentBufIndex = index;

    if(pInputBuffer) {
        m_postprocessor.start(mMemory, index, pInputChannel);
        ALOGD("%s: Post-process started", __func__);
        ALOGD("%s: Issue call to reprocess", __func__);
        m_postprocessor.processAuxiliaryData(pInputBuffer,pInputChannel);
    } else {
        m_postprocessor.start(mMemory, index, this);
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : dataNotifyCB
 *
 * DESCRIPTION: Channel Level callback used for super buffer data notify.
 *              This function is registered with mm-camera-interface to handle
 *              data notify
 *
 * PARAMETERS :
 *   @recvd_frame   : stream frame received
 *   userdata       : user data ptr
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3PicChannel::dataNotifyCB(mm_camera_super_buf_t *recvd_frame,
                                 void *userdata)
{
    ALOGV("%s: E\n", __func__);
    QCamera3PicChannel *channel = (QCamera3PicChannel *)userdata;

    if (channel == NULL) {
        ALOGE("%s: invalid channel pointer", __func__);
        return;
    }

    if(channel->m_numStreams != 1) {
        ALOGE("%s: Error: Bug: This callback assumes one stream per channel",__func__);
        return;
    }


    if(channel->mStreams[0] == NULL) {
        ALOGE("%s: Error: Invalid Stream object",__func__);
        return;
    }

    channel->QCamera3PicChannel::streamCbRoutine(recvd_frame, channel->mStreams[0]);

    ALOGV("%s: X\n", __func__);
    return;
}


int32_t QCamera3PicChannel::registerBuffers(uint32_t num_buffers,
                        buffer_handle_t **buffers)
{
    int rc = 0;
    cam_stream_type_t streamType;
    cam_format_t streamFormat;

    ALOGV("%s: E",__func__);
    rc = QCamera3PicChannel::initialize();
    if (rc < 0) {
        ALOGE("%s: init failed", __func__);
        return rc;
    }

    if (mCamera3Stream->format == HAL_PIXEL_FORMAT_BLOB) {
        streamType = CAM_STREAM_TYPE_NON_ZSL_SNAPSHOT;
        streamFormat = CAM_FORMAT_YUV_420_NV21;
    } else {
        //TODO: Fail for other types of streams for now
        ALOGE("%s: format is not BLOB", __func__);
        return -EINVAL;
    }
    /* Bookkeep buffer set because they go out of scope after register call */
    mNumBufs = num_buffers;
    mCamera3Buffers = new buffer_handle_t*[num_buffers];
    if (mCamera3Buffers == NULL) {
        ALOGE("%s: Failed to allocate buffer_handle_t*", __func__);
        return -ENOMEM;
    }
    for (size_t i = 0; i < num_buffers; i++)
        mCamera3Buffers[i] = buffers[i];

    ALOGV("%s: X",__func__);
    return rc;
}

void QCamera3PicChannel::streamCbRoutine(mm_camera_super_buf_t *super_frame,
                            QCamera3Stream *stream)
{
    //TODO
    //Used only for getting YUV. Jpeg callback will be sent back from channel
    //directly to HWI. Refer to func jpegEvtHandle

    //Got the yuv callback. Calling yuv callback handler in PostProc
    uint8_t frameIndex;
    mm_camera_super_buf_t* frame = NULL;
    if(!super_frame) {
         ALOGE("%s: Invalid Super buffer",__func__);
         return;
    }

    if(super_frame->num_bufs != 1) {
         ALOGE("%s: Multiple streams are not supported",__func__);
         return;
    }
    if(super_frame->bufs[0] == NULL ) {
         ALOGE("%s: Error, Super buffer frame does not contain valid buffer",
                  __func__);
         return;
    }

    frameIndex = (uint8_t)super_frame->bufs[0]->buf_idx;
    if(frameIndex >= mNumBufs) {
         ALOGE("%s: Error, Invalid index for buffer",__func__);
         if(stream) {
             stream->bufDone(frameIndex);
         }
         return;
    }

    frame = (mm_camera_super_buf_t *)malloc(sizeof(mm_camera_super_buf_t));
    if (frame == NULL) {
       ALOGE("%s: Error allocating memory to save received_frame structure.",
                                                                    __func__);
       if(stream) {
           stream->bufDone(frameIndex);
       }
       return;
    }
    *frame = *super_frame;

    m_postprocessor.processData(frame);
    free(super_frame);
    return;
}

QCamera3Memory* QCamera3PicChannel::getStreamBufs(uint32_t len)
{
    int rc = 0;

    if (mNumBufs == 0 || mCamera3Buffers == NULL) {
        ALOGE("%s: buffers not registered yet", __func__);
        return NULL;
    }

    if(mMemory) {
        delete mMemory;
        mMemory = NULL;
    }
    mMemory = new QCamera3GrallocMemory();
    if (mMemory == NULL) {
        return NULL;
    }

    //Registering Jpeg output buffer
    if (mMemory->registerBuffers(mNumBufs, mCamera3Buffers) < 0) {
        delete mMemory;
        mMemory = NULL;
        return NULL;
    }

    mYuvMemory = new QCamera3HeapMemory();
    if (!mYuvMemory) {
        ALOGE("%s: unable to create metadata memory", __func__);
        return NULL;
    }

    //Queue YUV buffers in the beginning mQueueAll = true
    rc = mYuvMemory->allocate(1, len, false);
    if (rc < 0) {
        ALOGE("%s: unable to allocate metadata memory", __func__);
        delete mYuvMemory;
        mYuvMemory = NULL;
        return NULL;
    }
    return mYuvMemory;
}

void QCamera3PicChannel::putStreamBufs()
{
    mMemory->unregisterBuffers();
    delete mMemory;
    mMemory = NULL;

    mYuvMemory->deallocate();
    delete mYuvMemory;
    mYuvMemory = NULL;
}

bool QCamera3PicChannel::isRawSnapshot()
{
   return !(mJpegSettings->is_jpeg_format);
}

int32_t QCamera3PicChannel::queueJpegSetting(uint32_t index, metadata_buffer_t *metadata)

{
    dim = mJpegSettings->thumbnail_size;
}

/*===========================================================================
 * FUNCTION   : getJpegQuality
 *
 * DESCRIPTION: get user set jpeg quality
 *
 * PARAMETERS : none
 *
 * RETURN     : jpeg quality setting
 *==========================================================================*/
int QCamera3PicChannel::getJpegQuality()
{
    int quality = mJpegSettings->jpeg_quality;
    if (quality < 0) {
        quality = 85;  //set to default quality value
    }
    return quality;
}

/*===========================================================================
 * FUNCTION   : getJpegRotation
 *
 * DESCRIPTION: get rotation information to be passed into jpeg encoding
 *
 * PARAMETERS : none
 *
 * RETURN     : rotation information
 *==========================================================================*/
int QCamera3PicChannel::getJpegRotation() {
    int rotation = mJpegSettings->jpeg_orientation;
    if (rotation < 0) {
        rotation = 0;
    }
    return rotation;
}

void QCamera3PicChannel::queueMetadata(mm_camera_super_buf_t *metadata_buf,
                                       QCamera3Channel *pMetaChannel,
                                       bool relinquish)
{
    if(relinquish)
        mMetaFrame = metadata_buf;
    m_pMetaChannel = pMetaChannel;
    m_postprocessor.processPPMetadata(metadata_buf);
}
/*===========================================================================
 * FUNCTION   : getRational
 *
 * DESCRIPTION: compose rational struct
 *
 * PARAMETERS :
 *   @rat     : ptr to struct to store rational info
 *   @num     :num of the rational
 *   @denom   : denom of the rational
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getRational(rat_t *rat, int num, int denom)
{
    if ((0 > num) || (0 > denom)) {
        ALOGE("%s: Negative values", __func__);
        return BAD_VALUE;
    }
    if (NULL == rat) {
        ALOGE("%s: NULL rat input", __func__);
        return BAD_VALUE;
    }
    rat->num = (uint32_t)num;
    rat->denom = (uint32_t)denom;
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : parseGPSCoordinate
 *
 * DESCRIPTION: parse GPS coordinate string
 *
 * PARAMETERS :
 *   @coord_str : [input] coordinate string
 *   @coord     : [output]  ptr to struct to store coordinate
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int parseGPSCoordinate(const char *coord_str, rat_t* coord)
{
    if(coord == NULL) {
        ALOGE("%s: error, invalid argument coord == NULL", __func__);
        return BAD_VALUE;
    }
    double degF = atof(coord_str);
    if (degF < 0) {
        degF = -degF;
    }
    double minF = (degF - (int) degF) * 60;
    double secF = (minF - (int) minF) * 60;

    getRational(&coord[0], (int)degF, 1);
    getRational(&coord[1], (int)minF, 1);
    getRational(&coord[2], (int)(secF * 10000), 10000);
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getExifDateTime
 *
 * DESCRIPTION: query exif date time
 *
 * PARAMETERS :
 *   @dateTime   : string to store exif date time
 *   @subsecTime : string to store exif subsec time
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/

int32_t getExifDateTime(String8 &dateTime, String8 &subsecTime)

{
    int32_t ret = NO_ERROR;

    //get time and date from system
    struct timeval tv;
    struct tm timeinfo_data;

    int res = gettimeofday(&tv, NULL);
    if (0 == res) {
        struct tm *timeinfo = localtime_r(&tv.tv_sec, &timeinfo_data);
        if (NULL != timeinfo) {
            //Write datetime according to EXIF Spec
            //"YYYY:MM:DD HH:MM:SS" (20 chars including \0)
            dateTime = String8::format("%04d:%02d:%02d %02d:%02d:%02d",
                    timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
                    timeinfo->tm_mday, timeinfo->tm_hour,
                    timeinfo->tm_min, timeinfo->tm_sec);
            //Write subsec according to EXIF Sepc
            subsecTime = String8::format("%06ld", tv.tv_usec);
        } else {
            ALOGE("%s: localtime_r() error", __func__);
            ret = UNKNOWN_ERROR;
        }
    } else if (-1 == res) {
        ALOGE("%s: gettimeofday() error: %s", __func__, strerror(errno));
        ret = UNKNOWN_ERROR;
    } else {
        ALOGE("%s: gettimeofday() unexpected return code: %d", __func__, res);
        ret = UNKNOWN_ERROR;
    }

    return ret;

}

/*===========================================================================
 * FUNCTION   : getExifFocalLength
 *
 * DESCRIPTION: get exif focal lenght
 *
 * PARAMETERS :
 *   @focalLength : ptr to rational strcut to store focal lenght
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifFocalLength(rat_t *focalLength, float value)
{
    int focalLengthValue =
        (int)(value * FOCAL_LENGTH_DECIMAL_PRECISION);
    return getRational(focalLength, focalLengthValue, FOCAL_LENGTH_DECIMAL_PRECISION);
}

/*===========================================================================
  * FUNCTION   : getExifExpTimeInfo
  *
  * DESCRIPTION: get exif exposure time information
  *
  * PARAMETERS :
  *   @expoTimeInfo     : expousure time value
  * RETURN     : nt32_t type of status
  *              NO_ERROR  -- success
  *              none-zero failure code
  *==========================================================================*/
int32_t getExifExpTimeInfo(rat_t *expoTimeInfo, int64_t value)
{

    int64_t cal_exposureTime;
    if (value != 0)
        cal_exposureTime = value;
    else
        cal_exposureTime = 60;

    return getRational(expoTimeInfo, 1, (int)cal_exposureTime);
}

/*===========================================================================
 * FUNCTION   : getExifGpsProcessingMethod
 *
 * DESCRIPTION: get GPS processing method
 *
 * PARAMETERS :
 *   @gpsProcessingMethod : string to store GPS process method
 *   @count               : lenght of the string
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifGpsProcessingMethod(char *gpsProcessingMethod,
                                   uint32_t &count, char* value)
{
    if(value != NULL) {
        memcpy(gpsProcessingMethod, ExifAsciiPrefix, EXIF_ASCII_PREFIX_SIZE);
        count = EXIF_ASCII_PREFIX_SIZE;
        strncpy(gpsProcessingMethod + EXIF_ASCII_PREFIX_SIZE, value, strlen(value));
        count += (uint32_t)strlen(value);
        gpsProcessingMethod[count++] = '\0'; // increase 1 for the last NULL char
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifLatitude
 *
 * DESCRIPTION: get exif latitude
 *
 * PARAMETERS :
 *   @latitude : ptr to rational struct to store latitude info
 *   @ladRef   : charater to indicate latitude reference
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifLatitude(rat_t *latitude,
                                           char *latRef, double value)
{
    char str[30];
    snprintf(str, sizeof(str), "%f", value);
    if(str != NULL) {
        parseGPSCoordinate(str, latitude);

        //set Latitude Ref
        float latitudeValue = strtof(str, 0);
        if(latitudeValue < 0.0f) {
            latRef[0] = 'S';
        } else {
            latRef[0] = 'N';
        }
        latRef[1] = '\0';
        return NO_ERROR;
    }else{
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifLongitude
 *
 * DESCRIPTION: get exif longitude
 *
 * PARAMETERS :
 *   @longitude : ptr to rational struct to store longitude info
 *   @lonRef    : charater to indicate longitude reference
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifLongitude(rat_t *longitude,
                                            char *lonRef, double value)
{
    char str[30];
    snprintf(str, sizeof(str), "%f", value);
    if(str != NULL) {
        parseGPSCoordinate(str, longitude);

        //set Longitude Ref
        float longitudeValue = strtof(str, 0);
        if(longitudeValue < 0.0f) {
            lonRef[0] = 'W';
        } else {
            lonRef[0] = 'E';
        }
        lonRef[1] = '\0';
        return NO_ERROR;
    }else{
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifAltitude
 *
 * DESCRIPTION: get exif altitude
 *
 * PARAMETERS :
 *   @altitude : ptr to rational struct to store altitude info
 *   @altRef   : charater to indicate altitude reference
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifAltitude(rat_t *altitude,
                                           char *altRef, double value)
{
    char str[30];
    snprintf(str, sizeof(str), "%f", value);
    if (str != NULL) {
        double value = atof(str);
        *altRef = 0;
        if(value < 0){
            *altRef = 1;
            value = -value;
        }
        return getRational(altitude, (int)(value * 1000), 1000);
    } else {
        return BAD_VALUE;
    }
}

/*===========================================================================
 * FUNCTION   : getExifGpsDateTimeStamp
 *
 * DESCRIPTION: get exif GPS date time stamp
 *
 * PARAMETERS :
 *   @gpsDateStamp : GPS date time stamp string
 *   @bufLen       : length of the string
 *   @gpsTimeStamp : ptr to rational struct to store time stamp info
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t getExifGpsDateTimeStamp(char *gpsDateStamp,
                                           uint32_t bufLen,
                                           rat_t *gpsTimeStamp, int64_t value)
{
    char str[30];
    snprintf(str, sizeof(str), "%lld", (long long int)value);
    if(str != NULL) {
        time_t unixTime = (time_t)atol(str);
        struct tm *UTCTimestamp = gmtime(&unixTime);

        strftime(gpsDateStamp, bufLen, "%Y:%m:%d", UTCTimestamp);

        getRational(&gpsTimeStamp[0], UTCTimestamp->tm_hour, 1);
        getRational(&gpsTimeStamp[1], UTCTimestamp->tm_min, 1);
        getRational(&gpsTimeStamp[2], UTCTimestamp->tm_sec, 1);

        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

int32_t getExifExposureValue(srat_t* exposure_val, int32_t exposure_comp,
                             cam_rational_type_t step)
{
    exposure_val->num = exposure_comp * step.numerator;
    exposure_val->denom = step.denominator;
    return 0;
}
/*===========================================================================
 * FUNCTION   : getExifData
 *
 * DESCRIPTION: get exif data to be passed into jpeg encoding
 *
 * PARAMETERS : none
 *
 * RETURN     : exif data from user setting and GPS
 *==========================================================================*/
QCamera3Exif *QCamera3PicChannel::getExifData()
{
    QCamera3Exif *exif = new QCamera3Exif();
    if (exif == NULL) {
        ALOGE("%s: No memory for QCamera3Exif", __func__);
        return NULL;
    }

    int32_t rc = NO_ERROR;
    uint32_t count = 0;

    // add exif entries
    String8 dateTime;
    String8 subsecTime;
    rc = getExifDateTime(dateTime, subsecTime);
    if (rc == NO_ERROR) {
        exif->addEntry(EXIFTAGID_DATE_TIME, EXIF_ASCII,
                (uint32_t)(dateTime.length() + 1), (void *)dateTime.string());
        exif->addEntry(EXIFTAGID_EXIF_DATE_TIME_ORIGINAL, EXIF_ASCII,
                (uint32_t)(dateTime.length() + 1), (void *)dateTime.string());
        exif->addEntry(EXIFTAGID_EXIF_DATE_TIME_DIGITIZED, EXIF_ASCII,
                (uint32_t)(dateTime.length() + 1), (void *)dateTime.string());
        exif->addEntry(EXIFTAGID_SUBSEC_TIME, EXIF_ASCII,
                (uint32_t)(subsecTime.length() + 1), (void *)subsecTime.string());
        exif->addEntry(EXIFTAGID_SUBSEC_TIME_ORIGINAL, EXIF_ASCII,
                (uint32_t)(subsecTime.length() + 1), (void *)subsecTime.string());
        exif->addEntry(EXIFTAGID_SUBSEC_TIME_DIGITIZED, EXIF_ASCII,
                (uint32_t)(subsecTime.length() + 1), (void *)subsecTime.string());

    } else {
        ALOGE("%s: getExifDateTime failed", __func__);
    }

    rat_t focalLength;
    rc = getExifFocalLength(&focalLength, mJpegSettings->lens_focal_length);
    if (rc == NO_ERROR) {
        exif->addEntry(EXIFTAGID_FOCAL_LENGTH,
                       EXIF_RATIONAL,
                       1,
                       (void *)&(focalLength));
    } else {
        ALOGE("%s: getExifFocalLength failed", __func__);
    }

    if (IS_PARAM_AVAILABLE(CAM_INTF_META_SENSOR_SENSITIVITY, metadata)) {
        int16_t isoSpeed = (int16_t)
                *(int32_t *)POINTER_OF_PARAM(CAM_INTF_META_SENSOR_SENSITIVITY, metadata);
        exif->addEntry(EXIFTAGID_ISO_SPEED_RATING,
                   EXIF_SHORT,
                   1,
                   (void *)&(isoSpeed));

    rat_t sensorExpTime ;
    rc = getExifExpTimeInfo(&sensorExpTime, (int64_t)mJpegSettings->sensor_exposure_time);
    if (rc == NO_ERROR){
        exif->addEntry(EXIFTAGID_EXPOSURE_TIME,
                       EXIF_RATIONAL,
                       1,
                       (void *)&(sensorExpTime));
    } else {
        ALOGE("%s: getExifExpTimeInfo failed", __func__);
    }

    if (strlen(jpeg_settings->gps_processing_method) > 0) {
        char gpsProcessingMethod[EXIF_ASCII_PREFIX_SIZE + GPS_PROCESSING_METHOD_SIZE];
        count = 0;
        rc = getExifGpsProcessingMethod(gpsProcessingMethod, count, mJpegSettings->gps_processing_method);
        if(rc == NO_ERROR) {
            exif->addEntry(EXIFTAGID_GPS_PROCESSINGMETHOD,
                           EXIF_ASCII,
                           count,
                           (void *)gpsProcessingMethod);
        } else {
            ALOGE("%s: getExifGpsProcessingMethod failed", __func__);
        }
    }

    if (mJpegSettings->gps_coordinates[0]) {
        rat_t latitude[3];
        char latRef[2];
        rc = getExifLatitude(latitude, latRef, *(mJpegSettings->gps_coordinates[0]));
        if(rc == NO_ERROR) {
            exif->addEntry(EXIFTAGID_GPS_LATITUDE,
                           EXIF_RATIONAL,
                           3,
                           (void *)latitude);
            exif->addEntry(EXIFTAGID_GPS_LATITUDE_REF,
                           EXIF_ASCII,
                           2,
                           (void *)latRef);
        } else {
            ALOGE("%s: getExifLatitude failed", __func__);
        }
    }

    if (mJpegSettings->gps_coordinates[1]) {
        rat_t longitude[3];
        char lonRef[2];
        rc = getExifLongitude(longitude, lonRef, *(mJpegSettings->gps_coordinates[1]));
        if(rc == NO_ERROR) {
            exif->addEntry(EXIFTAGID_GPS_LONGITUDE,
                           EXIF_RATIONAL,
                           3,
                           (void *)longitude);

            exif->addEntry(EXIFTAGID_GPS_LONGITUDE_REF,
                           EXIF_ASCII,
                           2,
                           (void *)lonRef);
        } else {
            ALOGE("%s: getExifLongitude failed", __func__);
        }
    }

    if (mJpegSettings->gps_coordinates[2]) {
        rat_t altitude;
        char altRef;
        rc = getExifAltitude(&altitude, &altRef, *(mJpegSettings->gps_coordinates[2]));
        if(rc == NO_ERROR) {
            exif->addEntry(EXIFTAGID_GPS_ALTITUDE,
                           EXIF_RATIONAL,
                           1,
                           (void *)&(altitude));

            exif->addEntry(EXIFTAGID_GPS_ALTITUDE_REF,
                           EXIF_BYTE,
                           1,
                           (void *)&altRef);
        } else {
            ALOGE("%s: getExifAltitude failed", __func__);
        }
    }

    if (mJpegSettings->gps_timestamp) {
        char gpsDateStamp[20];
        rat_t gpsTimeStamp[3];
        rc = getExifGpsDateTimeStamp(gpsDateStamp, 20, gpsTimeStamp, *(mJpegSettings->gps_timestamp));
        if(rc == NO_ERROR) {
            exif->addEntry(EXIFTAGID_GPS_DATESTAMP, EXIF_ASCII,
                    (uint32_t)(strlen(gpsDateStamp) + 1), (void *)gpsDateStamp);

            exif->addEntry(EXIFTAGID_GPS_TIMESTAMP,
                           EXIF_RATIONAL,
                           3,
                           (void *)gpsTimeStamp);
        } else {
            ALOGE("%s: getExifGpsDataTimeStamp failed", __func__);
        }
    }

    srat_t exposure_val;
    rc = getExifExposureValue(&exposure_val, mJpegSettings->exposure_compensation,
                              mJpegSettings->exposure_comp_step);
    if(rc == NO_ERROR) {
        exif->addEntry(EXIFTAGID_EXPOSURE_BIAS_VALUE,
                       EXIF_SRATIONAL,
                       1,
                       (void *)(&exposure_val));
    } else {
        ALOGE("%s: getExifExposureValue failed ", __func__);
    }

    char value[PROPERTY_VALUE_MAX];
    if (property_get("ro.product.manufacturer", value, "QCOM-AA") > 0) {
        exif->addEntry(EXIFTAGID_MAKE, EXIF_ASCII,
                (uint32_t)(strlen(value) + 1), (void *)value);
    } else {
        ALOGE("%s: getExifMaker failed", __func__);
    }

    if (property_get("ro.product.model", value, "QCAM-AA") > 0) {
        exif->addEntry(EXIFTAGID_MODEL, EXIF_ASCII,
                (uint32_t)(strlen(value) + 1), (void *)value);
    } else {
        ALOGE("%s: getExifModel failed", __func__);
    }

    return exif;
}

/* There can be MAX_INFLIGHT_REQUESTS number of requests that could get queued up. Hence
 allocating same number of picture channel buffers */
uint32_t QCamera3PicChannel::kMaxBuffers = MAX_INFLIGHT_REQUESTS;

void QCamera3PicChannel::overrideYuvSize(uint32_t width, uint32_t height)
{
   mYuvWidth = width;
   mYuvHeight = height;
}

/*===========================================================================
 * FUNCTION   : QCamera3ReprocessChannel
 *
 * DESCRIPTION: constructor of QCamera3ReprocessChannel
 *
 * PARAMETERS :
 *   @cam_handle : camera handle
 *   @cam_ops    : ptr to camera ops table
 *   @pp_mask    : post-proccess feature mask
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3ReprocessChannel::QCamera3ReprocessChannel(uint32_t cam_handle,
                                                 mm_camera_ops_t *cam_ops,
                                                 channel_cb_routine cb_routine,
                                                 cam_padding_info_t *paddingInfo,
                                                 void *userData, void *ch_hdl) :
    QCamera3Channel(cam_handle, cam_ops, cb_routine, paddingInfo, userData),
    picChHandle(ch_hdl),
    m_pSrcChannel(NULL),
    m_pMetaChannel(NULL),
    mMemory(NULL)
{
    memset(mSrcStreamHandles, 0, sizeof(mSrcStreamHandles));
}


/*===========================================================================
 * FUNCTION   : QCamera3ReprocessChannel
 *
 * DESCRIPTION: constructor of QCamera3ReprocessChannel
 *
 * PARAMETERS :
 *   @cam_handle : camera handle
 *   @cam_ops    : ptr to camera ops table
 *   @pp_mask    : post-proccess feature mask
 *
 * RETURN     : none
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::initialize()
{
    int32_t rc = NO_ERROR;
    mm_camera_channel_attr_t attr;

    memset(&attr, 0, sizeof(mm_camera_channel_attr_t));
    attr.notify_mode = MM_CAMERA_SUPER_BUF_NOTIFY_CONTINUOUS;
    attr.max_unmatched_frames = 1;

    rc = init(&attr, NULL);
    if (rc < 0) {
        ALOGE("%s: init failed", __func__);
    }
    return rc;
}


/*===========================================================================
 * FUNCTION   : QCamera3ReprocessChannel
 *
 * DESCRIPTION: constructor of QCamera3ReprocessChannel
 *
 * PARAMETERS :
 *   @cam_handle : camera handle
 *   @cam_ops    : ptr to camera ops table
 *   @pp_mask    : post-proccess feature mask
 *
 * RETURN     : none
 *==========================================================================*/
void QCamera3ReprocessChannel::streamCbRoutine(mm_camera_super_buf_t *super_frame,
                                  QCamera3Stream *stream)
{
    //Got the pproc data callback. Now send to jpeg encoding
    uint8_t frameIndex;
    mm_camera_super_buf_t* frame = NULL;
    QCamera3PicChannel *obj = (QCamera3PicChannel *)picChHandle;

    if(!super_frame) {
         ALOGE("%s: Invalid Super buffer",__func__);
         return;
    }

    if(super_frame->num_bufs != 1) {
         ALOGE("%s: Multiple streams are not supported",__func__);
         return;
    }
    if(super_frame->bufs[0] == NULL ) {
         ALOGE("%s: Error, Super buffer frame does not contain valid buffer",
                  __func__);
         return;
    }

    frameIndex = (uint8_t)super_frame->bufs[0]->buf_idx;
    frame = (mm_camera_super_buf_t *)malloc(sizeof(mm_camera_super_buf_t));
    if (frame == NULL) {
       ALOGE("%s: Error allocating memory to save received_frame structure.",
                                                                    __func__);
       if(stream) {
           stream->bufDone(frameIndex);
       }
       return;
    }
    *frame = *super_frame;
    obj->m_postprocessor.processPPData(frame);
    return;
}

/*===========================================================================
 * FUNCTION   : QCamera3ReprocessChannel
 *
 * DESCRIPTION: default constructor of QCamera3ReprocessChannel
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3ReprocessChannel::QCamera3ReprocessChannel() :
    m_pSrcChannel(NULL),
    m_pMetaChannel(NULL)
{
}

/*===========================================================================
 * FUNCTION   : QCamera3ReprocessChannel
 *
 * DESCRIPTION: register the buffers of the reprocess channel
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::registerBuffers(
    uint32_t /*num_buffers*/, buffer_handle_t ** /*buffers*/)
{
   return 0;
}

/*===========================================================================
 * FUNCTION   : getStreamBufs
 *
 * DESCRIPTION: register the buffers of the reprocess channel
 *
 * PARAMETERS : none
 *
 * RETURN     : QCamera3Memory *
 *==========================================================================*/
QCamera3Memory* QCamera3ReprocessChannel::getStreamBufs(uint32_t len)
{
   int rc = 0;

    mMemory = new QCamera3HeapMemory();
    if (!mMemory) {
        ALOGE("%s: unable to create reproc memory", __func__);
        return NULL;
    }

    //Queue YUV buffers in the beginning mQueueAll = true
    rc = mMemory->allocate(2, len, true);
    if (rc < 0) {
        ALOGE("%s: unable to allocate reproc memory", __func__);
        delete mMemory;
        mMemory = NULL;
        return NULL;
    }
    return mMemory;
}

/*===========================================================================
 * FUNCTION   : getStreamBufs
 *
 * DESCRIPTION: register the buffers of the reprocess channel
 *
 * PARAMETERS : none
 *
 * RETURN     :
 *==========================================================================*/
void QCamera3ReprocessChannel::putStreamBufs()
{
    mMemory->deallocate();
    delete mMemory;
    mMemory = NULL;
}

/*===========================================================================
 * FUNCTION   : ~QCamera3ReprocessChannel
 *
 * DESCRIPTION: destructor of QCamera3ReprocessChannel
 *
 * PARAMETERS : none
 *
 * RETURN     : none
 *==========================================================================*/
QCamera3ReprocessChannel::~QCamera3ReprocessChannel()
{
}

/*===========================================================================
 * FUNCTION   : getStreamBySourceHandle
 *
 * DESCRIPTION: find reprocess stream by its source stream handle
 *
 * PARAMETERS :
 *   @srcHandle : source stream handle
 *
 * RETURN     : ptr to reprocess stream if found. NULL if not found
 *==========================================================================*/
QCamera3Stream * QCamera3ReprocessChannel::getStreamBySourceHandle(uint32_t srcHandle)
{
    QCamera3Stream *pStream = NULL;

    for (uint32_t i = 0; i < m_numStreams; i++) {
        if (mSrcStreamHandles[i] == srcHandle) {
            pStream = mStreams[i];
            break;
        }
    }
    return pStream;
}

/*===========================================================================
 * FUNCTION   : getSrcStreamBySrcHandle
 *
 * DESCRIPTION: find source stream by source stream handle
 *
 * PARAMETERS :
 *   @srcHandle : source stream handle
 *
 * RETURN     : ptr to reprocess stream if found. NULL if not found
 *==========================================================================*/
QCamera3Stream * QCamera3ReprocessChannel::getSrcStreamBySrcHandle(uint32_t srcHandle)
{
    QCamera3Stream *pStream = NULL;

    if (NULL == m_pSrcChannel) {
        return NULL;
    }

    for (uint32_t i = 0; i < m_numStreams; i++) {
        if (mSrcStreamHandles[i] == srcHandle) {
            pStream = m_pSrcChannel->getStreamByIndex(i);
            break;
        }
    }
    return pStream;
}

/*===========================================================================
 * FUNCTION   : stop
 *
 * DESCRIPTION: stop channel
 *
 * PARAMETERS : none
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::stop()
{
    unmapOfflineBuffers(true);

    return QCamera3Channel::stop();
}

/*===========================================================================
 * FUNCTION   : unmapOfflineBuffers
 *
 * DESCRIPTION: Unmaps offline buffers
 *
 * PARAMETERS : none
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::unmapOfflineBuffers(bool all)
{
    int rc = NO_ERROR;
    if (!mOfflineBuffers.empty()) {
        QCamera3Stream *stream = NULL;
        List<OfflineBuffer>::iterator it = mOfflineBuffers.begin();
        for (; it != mOfflineBuffers.end(); it++) {
           stream = (*it).stream;
           if (NULL != stream) {
               rc = stream->unmapBuf((*it).type,
                                     (*it).index,
                                        -1);
               if (NO_ERROR != rc) {
                   ALOGE("%s: Error during offline buffer unmap %d",
                         __func__, rc);
               }
               CDBG("%s: Unmapped buffer with index %d", __func__, (*it).index);
           }
           if (!all) {
               mOfflineBuffers.erase(it);
               break;
           }
        }
        if (all) {
           mOfflineBuffers.clear();
        }
    }

    if (!mOfflineMetaBuffers.empty()) {
        QCamera3Stream *stream = NULL;
        List<OfflineBuffer>::iterator it = mOfflineMetaBuffers.begin();
        for (; it != mOfflineBuffers.end(); it++) {
           stream = (*it).stream;
           if (NULL != stream) {
               rc = stream->unmapBuf((*it).type,
                                     (*it).index,
                                        -1);
               if (NO_ERROR != rc) {
                   ALOGE("%s: Error during offline buffer unmap %d",
                         __func__, rc);
               }
               CDBG("%s: Unmapped meta buffer with index %d", __func__, (*it).index);
           }
           if (!all) {
               mOfflineMetaBuffers.erase(it);
               break;
           }
        }
        if (all) {
           mOfflineMetaBuffers.clear();
        }
    }
    return rc;
}


/*===========================================================================
 * FUNCTION   : extractFrameAndCrop
 *
 * DESCRIPTION: Extract output crop and frame data if present
 *
 * PARAMETERS :
 *   @frame     : input frame from source stream
 *   meta_buffer: metadata buffer
 *   @metadata  : corresponding metadata
 *   @fwk_frame :
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::extractFrameAndCrop(mm_camera_super_buf_t *frame,
        mm_camera_buf_def_t *meta_buffer, metadata_buffer_t *metadata, qcamera_fwk_input_pp_data_t &fwk_frame)
{
    if ((NULL == meta_buffer) || (NULL == frame) || (NULL == metadata)) {
        return BAD_VALUE;
    }

    for (uint32_t i = 0; i < frame->num_bufs; i++) {

        QCamera3Stream *pStream = getStreamBySrcHandle(frame->bufs[i]->stream_id);
        QCamera3Stream *pSrcStream = getSrcStreamBySrcHandle(frame->bufs[i]->stream_id);

            if (pStream != NULL && pSrcStream != NULL) {
                // Find crop info for reprocess stream
                cam_crop_data_t *crop_data = (cam_crop_data_t *)
                        POINTER_OF_PARAM(CAM_INTF_META_CROP_DATA, metadata);
                if (NULL != crop_data) {
                    for (int j = 0; j < crop_data->num_of_streams; j++) {
                        if (crop_data->crop_info[j].stream_id ==
                               pSrcStream->getMyServerID()) {
                            fwk_frame.reproc_config.output_crop =
                                    crop_data->crop_info[j].crop;
                            CDBG("%s: Found reprocess crop %dx%d %dx%d", __func__,
                                    crop_data->crop_info[0].crop.left,
                                    crop_data->crop_info[0].crop.top,
                                    crop_data->crop_info[0].crop.width,
                                    crop_data->crop_info[0].crop.height);
                            break;
                        }
                    }
                    fwk_frame.input_buffer = *frame->bufs[i];
                    fwk_frame.metadata_buffer = *meta_buffer;
                    break;
                } else {
                    continue;
                }
            } else {
                ALOGE("%s: Source/Re-process streams are invalid", __func__);
                return BAD_VALUE;
        }
    }

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : extractCrop
 *
 * DESCRIPTION: buf done method for a metadata buffer
 *
 * PARAMETERS :
 *   @recvd_frame : received metadata frame
 *
 * RETURN     :
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::metadataBufDone(mm_camera_super_buf_t *recvd_frame)
{
   int32_t rc;
   rc = ((QCamera3MetadataChannel*)m_pMetaChannel)->bufDone(recvd_frame);
   free(recvd_frame);
   recvd_frame = NULL;
   return rc;
}

/*===========================================================================
 * FUNCTION   : doReprocess
 *
 * DESCRIPTION: request to do a reprocess on the frame
 *
 * PARAMETERS :
 *   @frame   : frame to be performed a reprocess
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::doReprocess(mm_camera_super_buf_t *frame,
                                              mm_camera_super_buf_t *meta_frame)
{
    int32_t rc = 0;
    if (m_numStreams < 1) {
        ALOGE("%s: No reprocess stream is created", __func__);
        return -1;
    }

    if (NULL == frame) {
        ALOGE("%s: Incorrect input frame", __func__);
        return BAD_VALUE;
    }

    if (NULL == frame->metadata_buffer.buffer) {
        ALOGE("%s: No metadata available", __func__);
        return BAD_VALUE;
    }

    if (NULL == frame->input_buffer.buffer) {
        ALOGE("%s: No input buffer available", __func__);
        return BAD_VALUE;
    }

    if ((0 == m_numStreams) || (NULL == mStreams[0])) {
        ALOGE("%s: Reprocess stream not initialized!", __func__);
        return NO_INIT;
    }

    QCamera3Stream *pStream = mStreams[0];
    int32_t max_idx = MAX_INFLIGHT_REQUESTS-1;
    //loop back the indices if max burst count reached
    if (mOfflineBuffersIndex == max_idx) {
       mOfflineBuffersIndex = -1;
    }
    uint32_t buf_idx = (uint32_t)(mOfflineBuffersIndex + 1);
    rc = pStream->mapBuf(
            CAM_MAPPING_BUF_TYPE_OFFLINE_INPUT_BUF,
            buf_idx, -1,
            frame->input_buffer.fd, frame->input_buffer.frame_len);
    if (NO_ERROR == rc) {
        mappedBuffer.index = buf_idx;
        mappedBuffer.stream = pStream;
        mappedBuffer.type = CAM_MAPPING_BUF_TYPE_OFFLINE_INPUT_BUF;
        mOfflineBuffers.push_back(mappedBuffer);
        mOfflineBuffersIndex = (int32_t)buf_idx;
        CDBG("%s: Mapped buffer with index %d", __func__, mOfflineBuffersIndex);
    }

    max_idx = MAX_INFLIGHT_REQUESTS*2 - 1;
    //loop back the indices if max burst count reached
    if (mOfflineMetaIndex == max_idx) {
       mOfflineMetaIndex = MAX_INFLIGHT_REQUESTS-1;
    }
    uint32_t meta_buf_idx = (uint32_t)(mOfflineMetaIndex + 1);
    rc |= pStream->mapBuf(
            CAM_MAPPING_BUF_TYPE_OFFLINE_META_BUF,
            meta_buf_idx, -1,
            frame->metadata_buffer.fd, frame->metadata_buffer.frame_len);
    if (NO_ERROR == rc) {
        mappedBuffer.index = meta_buf_idx;
        mappedBuffer.stream = pStream;
        mappedBuffer.type = CAM_MAPPING_BUF_TYPE_OFFLINE_META_BUF;
        mOfflineMetaBuffers.push_back(mappedBuffer);
        mOfflineMetaIndex = (int32_t)meta_buf_idx;
        CDBG("%s: Mapped meta buffer with index %d", __func__, mOfflineMetaIndex);

    }
    for (int i = 0; i < frame->num_bufs; i++) {
        QCamera3Stream *pStream = getStreamBySourceHandle(frame->bufs[i]->stream_id);
        if (pStream != NULL) {
            cam_stream_parm_buffer_t param;
            memset(&param, 0, sizeof(cam_stream_parm_buffer_t));
            param.type = CAM_STREAM_PARAM_TYPE_DO_REPROCESS;
            param.reprocess.buf_index = frame->bufs[i]->buf_idx;
            param.reprocess.frame_idx = frame->bufs[i]->frame_idx;
            if (meta_frame != NULL) {
               param.reprocess.meta_present = 1;
               param.reprocess.meta_stream_handle = m_pMetaChannel->mStreams[0]->getMyServerID();
               param.reprocess.meta_buf_index = meta_frame->bufs[0]->buf_idx;
            }
            rc = pStream->setParameter(param);
            if (rc != NO_ERROR) {
                ALOGE("%s: stream setParameter for reprocess failed", __func__);
                break;
            }
        }
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : doReprocess
 *
 * DESCRIPTION: request to do a reprocess on the frame
 *
 * PARAMETERS :
 *   @buf_fd     : fd to the input buffer that needs reprocess
 *   @buf_lenght : length of the input buffer
 *   @ret_val    : result of reprocess.
 *                 Example: Could be faceID in case of register face image.
 *   @meta_frame : metadata frame.
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::doReprocess(int buf_fd, size_t buf_length,
        int32_t &ret_val, mm_camera_super_buf_t *meta_frame)
{
    int32_t rc = 0;
    if (m_numStreams < 1) {
        ALOGE("%s: No reprocess stream is created", __func__);
        return -1;
    }
    if (meta_frame == NULL) {
        ALOGE("%s: Did not get corresponding metadata in time", __func__);
        return -1;
    }

    uint8_t buf_idx = 0;
    for (uint32_t i = 0; i < m_numStreams; i++) {
        rc = mStreams[i]->mapBuf(CAM_MAPPING_BUF_TYPE_OFFLINE_INPUT_BUF,
                                 buf_idx, -1,
                                 buf_fd, buf_length);

        if (rc == NO_ERROR) {
            cam_stream_parm_buffer_t param;
            memset(&param, 0, sizeof(cam_stream_parm_buffer_t));
            param.type = CAM_STREAM_PARAM_TYPE_DO_REPROCESS;
            param.reprocess.buf_index = buf_idx;
            param.reprocess.meta_present = 1;
            param.reprocess.meta_stream_handle = m_pMetaChannel->mStreams[0]->getMyServerID();
            param.reprocess.meta_buf_index = meta_frame->bufs[0]->buf_idx;
            rc = mStreams[i]->setParameter(param);
            if (rc == NO_ERROR) {
                ret_val = param.reprocess.ret_val;
            }
            mStreams[i]->unmapBuf(CAM_MAPPING_BUF_TYPE_OFFLINE_INPUT_BUF,
                                  buf_idx, -1);
        }
    }
    return rc;
}

/*===========================================================================
 * FUNCTION   : addReprocStreamsFromSource
 *
 * DESCRIPTION: add reprocess streams from input source channel
 *
 * PARAMETERS :
 *   @config         : pp feature configuration
 *   @pSrcChannel    : ptr to input source channel that needs reprocess
 *   @pMetaChannel   : ptr to metadata channel to get corresp. metadata
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCamera3ReprocessChannel::addReprocStreamsFromSource(cam_pp_feature_config_t &config,
                                                             QCamera3Channel *pSrcChannel,
                                                             QCamera3Channel *pMetaChannel)
{
    int32_t rc = 0;
    QCamera3Stream *pSrcStream = pSrcChannel->getStreamByIndex(0);
    if (pSrcStream == NULL) {
       ALOGE("%s: source channel doesn't have a stream", __func__);
       return BAD_VALUE;
    }
    cam_stream_reproc_config_t reprocess_config;
    cam_dimension_t streamDim;
    cam_stream_type_t streamType;
    cam_format_t streamFormat;
    cam_frame_len_offset_t frameOffset;
    int num_buffers = 2;

    streamType = CAM_STREAM_TYPE_OFFLINE_PROC;
    reprocess_config.pp_type = CAM_OFFLINE_REPROCESS_TYPE;

    reprocess_config.offline.input_fmt = src_config.stream_format;
    reprocess_config.offline.input_dim = src_config.input_stream_dim;
    reprocess_config.offline.input_buf_planes.plane_info =
            src_config.input_stream_plane_info.plane_info;
    reprocess_config.offline.num_of_bufs = (uint8_t)num_buffers;
    reprocess_config.offline.input_type = src_config.stream_type;

    reprocess_config.pp_type = CAM_ONLINE_REPROCESS_TYPE;
    reprocess_config.online.input_stream_id = pSrcStream->getMyServerID();
    reprocess_config.online.input_stream_type = pSrcStream->getMyType();
    reprocess_config.pp_feature_config = config;

    mSrcStreamHandles[m_numStreams] = pSrcStream->getMyHandle();

    if (reprocess_config.pp_feature_config.feature_mask & CAM_QCOM_FEATURE_ROTATION) {
        if (reprocess_config.pp_feature_config.rotation == ROTATE_90 ||
            reprocess_config.pp_feature_config.rotation == ROTATE_270) {
            // rotated by 90 or 270, need to switch width and height
            int32_t temp = streamDim.height;
            streamDim.height = streamDim.width;
            streamDim.width = temp;
        }
    }

    QCamera3Stream *pStream = new QCamera3Stream(m_camHandle,
                                               m_handle,
                                               m_camOps,
                                               mPaddingInfo,
                                               (QCamera3Channel*)this);
    if (pStream == NULL) {
        ALOGE("%s: No mem for Stream", __func__);
        return NO_MEMORY;
    }

    rc = pStream->init(streamType, src_config.stream_format,
            streamDim, &reprocess_config,
            (uint8_t)num_buffers,
            reprocess_config.pp_feature_config.feature_mask,
            QCamera3Channel::streamCbRoutine, this);

    if (rc == 0) {
        mStreams[m_numStreams] = pStream;
        m_numStreams++;
    } else {
        ALOGE("%s: failed to create reprocess stream", __func__);
        delete pStream;
    }

    if (rc == NO_ERROR) {
        m_pSrcChannel = pSrcChannel;
        m_pMetaChannel = pMetaChannel;
    }
    if(m_camOps->request_super_buf(m_camHandle,m_handle,1) < 0) {
        ALOGE("%s: Request for super buffer failed",__func__);
    }
    return rc;
}


}; // namespace qcamera
