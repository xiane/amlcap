// ffmpeg -framerate 60 -i test.hevc -vcodec copy test.mp4
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <cstdlib> //atoi

#include <exception>
#include <vector>
#include <cstring>

#include "libvphevcodec/vp_hevc_codec_1_0.h"

#include <linux/videodev2.h> // V4L
#include <sys/mman.h>	// mmap

#include "Exception.h"
#include "Stopwatch.h"
#include "Timer.h"
#include "Mutex.h"

const char* DEFAULT_DEVICE = "/dev/video0";
const char* DEFAULT_OUTPUT = "default.hevc";
const int BUFFER_COUNT = 8;

const int DEFAULT_WIDTH = 640;
const int DEFAULT_HEIGHT = 480;
const int DEFAULT_FRAME_RATE = 30;
const int DEFAULT_BITRATE = 1000000 * 10;

struct BufferMapping
{
	void* Start;
	size_t Length;
};

struct option longopts[] = {
	{ "device",			required_argument,	NULL,	'd' },
	{ "output",			required_argument,	NULL,	'o' },
	{ "width",			required_argument,	NULL,	'w' },
	{ "height",			required_argument,	NULL,	'h' },
	{ "fps",			required_argument,	NULL,	'f' },
	{ "bitrate",		required_argument,	NULL,	'b' },
	{ 0, 0, 0, 0 }
};

vl_codec_handle_t handle;
vl_img_format_t img_format;
int encoderFileDescriptor = -1;
unsigned char* encodeInBuffer = nullptr;
unsigned char* encodeBitstreamBuffer = nullptr;
size_t encodeBitstreamBufferLength = 0;
Mutex encodeMutex;
double timeStamp = 0;
double frameRate = 0;

Stopwatch sw;
Timer timer;

void EncodeFrame()
{
	encodeMutex.Lock();

	// Encode the video frames
	vl_frame_type_t type = FRAME_TYPE_AUTO;
	unsigned char* in = encodeInBuffer;
	int in_size = encodeBitstreamBufferLength;
	unsigned char* out = encodeBitstreamBuffer;
	int outCnt = vl_video_encoder_encode(handle, type, in, in_size, out, img_format);

	encodeMutex.Unlock();

	if (outCnt > 0)
	{
		ssize_t writeCount = write(encoderFileDescriptor, encodeBitstreamBuffer, outCnt);
		if (writeCount < 0)
		{
			throw Exception("write failed.");
		}
	}

	timeStamp += frameRate;
}

int main(int argc, char** argv)
{
	int io;

	// options
	const char* device = DEFAULT_DEVICE;
	const char* output = DEFAULT_OUTPUT;
	int width = DEFAULT_WIDTH;
	int height = DEFAULT_HEIGHT;
	int fps = DEFAULT_FRAME_RATE;
	int bitrate = DEFAULT_BITRATE;

	int c;
	while ((c = getopt_long(argc, argv, "d:o:w:h:f:b:", longopts, NULL)) != -1)
	{
		switch (c)
		{
			case 'd':
				device = optarg;
				break;

			case 'o':
				output = optarg;
				break;

			case 'w':
				width = atoi(optarg);
				break;

			case 'h':
				height = atoi(optarg);
				break;

			case 'f':
				fps = atoi(optarg);
				break;

			case 'b':
				bitrate = atoi(optarg);
				break;
			default:
				throw Exception("Unknown option.");
		}
	}

	int captureDev = open(device, O_RDWR);
	if (captureDev < 0)
	{
		throw Exception("capture device open failed.");
	}

	v4l2_capability caps = { 0 };
	io = ioctl(captureDev, VIDIOC_QUERYCAP, &caps);
	if (io < 0)
	{
		throw Exception("VIDIOC_QUERYCAP failed.");
	}

	printf("card = %s\n", (char*)caps.card);
	printf("\tbus_info = %s\n", (char*)caps.bus_info);
	printf("\tdriver = %s\n", (char*)caps.driver);

	if (!caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)
	{
		throw Exception("V4L2_CAP_VIDEO_CAPTURE not supported.");
	}
	else
	{
		fprintf(stderr, "V4L2_CAP_VIDEO_CAPTURE supported.\n");
	}

	v4l2_fmtdesc formatDesc = { 0 };
	formatDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	
	fprintf(stderr, "Supported formats:\n");
	while (true)
	{
		io = ioctl(captureDev, VIDIOC_ENUM_FMT, &formatDesc);
		if (io < 0)
		{
			//printf("VIDIOC_ENUM_FMT failed.\n");
			break;
		}
		
		fprintf(stderr, "\tdescription = %s, pixelformat=0x%x\n", formatDesc.description, formatDesc.pixelformat);

		
		v4l2_frmsizeenum formatSize = { 0 };
		formatSize.pixel_format = formatDesc.pixelformat;

		while (true)
		{
			io = ioctl(captureDev, VIDIOC_ENUM_FRAMESIZES, &formatSize);
			if (io < 0)
			{
				//printf("VIDIOC_ENUM_FRAMESIZES failed.\n");
				break;
			}

			fprintf(stderr, "\t\twidth = %d, height = %d\n", formatSize.discrete.width, formatSize.discrete.height);

			v4l2_frmivalenum frameInterval = { 0 };
			frameInterval.pixel_format = formatSize.pixel_format;
			frameInterval.width = formatSize.discrete.width;
			frameInterval.height = formatSize.discrete.height;

			while (true)
			{
				io = ioctl(captureDev, VIDIOC_ENUM_FRAMEINTERVALS, &frameInterval);
				if (io < 0)
				{
					//printf("VIDIOC_ENUM_FRAMEINTERVALS failed.\n");
					break;
				}

				fprintf(stderr, "\t\t\tnumerator = %d, denominator = %d\n", frameInterval.discrete.numerator, frameInterval.discrete.denominator);
				++frameInterval.index;
			}
			++formatSize.index;
		}
		++formatDesc.index;
	}
	
	// TODO: format selection from user input / enumeration

	v4l2_format format = { 0 };
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = width;
	format.fmt.pix.height = height;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	//format.fmt.pix.field = V4L2_FIELD_ANY;

	io = ioctl(captureDev, VIDIOC_S_FMT, &format);
	if (io < 0)
	{
		throw Exception("VIDIOC_S_FMT failed.");
	}

	fprintf(stderr, "v4l2_format: width=%d, height=%d, pixelformat=0x%x\n",
		format.fmt.pix.width, format.fmt.pix.height, format.fmt.pix.pixelformat);

	v4l2_streamparm streamParm = { 0 };
	streamParm.type = format.type;
	streamParm.parm.capture.timeperframe.numerator = 1;
	streamParm.parm.capture.timeperframe.denominator = fps;

	io = ioctl(captureDev, VIDIOC_S_PARM, &streamParm);
	if (io < 0)
	{
		throw Exception("VIDIOC_S_PARM failed.");
	}

	fprintf(stderr, "capture.timeperframe: numerator=%d, denominator=%d\n",
		streamParm.parm.capture.timeperframe.numerator,
		streamParm.parm.capture.timeperframe.denominator);

	// Request buffers
	v4l2_requestbuffers requestBuffers = { 0 };
	requestBuffers.count = BUFFER_COUNT;
	requestBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	requestBuffers.memory = V4L2_MEMORY_MMAP;

	io = ioctl(captureDev, VIDIOC_REQBUFS, &requestBuffers);
	if (io < 0)
	{
		throw Exception("VIDIOC_REQBUFS failed.");
	}

	// Map buffers
	BufferMapping bufferMappings[requestBuffers.count] = { 0 };
	for (int i = 0; i < requestBuffers.count; ++i)
	{
		v4l2_buffer buffer = { 0 };
		buffer.type = requestBuffers.type;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;

		io = ioctl(captureDev, VIDIOC_QUERYBUF, &buffer);
		if (io < 0)
		{
			throw Exception("VIDIOC_QUERYBUF failed.");
		}

		bufferMappings[i].Length = buffer.length;
		bufferMappings[i].Start = mmap(NULL, buffer.length,
			PROT_READ | PROT_WRITE, /* recommended */
			MAP_SHARED,             /* recommended */
			captureDev, buffer.m.offset);
	}

	// Queue buffers
	for (int i = 0; i < requestBuffers.count; ++i)
	{
		v4l2_buffer buffer = { 0 };
		buffer.index = i;
		buffer.type = requestBuffers.type;
		buffer.memory = requestBuffers.memory;

		io = ioctl(captureDev, VIDIOC_QBUF, &buffer);
		if (io < 0)
		{
			throw Exception("VIDIOC_QBUF failed.");
		}
	}

	// Create an output file
	int fdOut;

	if (std::strcmp(output, "-") == 0)
	{
		fdOut = 1; //stdout
	}
	else
	{
		mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

		fdOut = open(output, O_CREAT | O_TRUNC | O_WRONLY, mode);
		if (fdOut < 0)
		{
			throw Exception("open output failed.");
		}
	}

	encoderFileDescriptor = fdOut;

	// Initialize the encoder
	vl_codec_id_t codec_id = CODEC_ID_H265;
	width = format.fmt.pix.width;
	height = format.fmt.pix.height;
	fps = (int)((double)streamParm.parm.capture.timeperframe.denominator /
				(double)streamParm.parm.capture.timeperframe.numerator);
	//int bit_rate = bitrate;
	int gop = 10;

	fprintf(stderr, "vl_video_encoder_init: width=%d, height=%d, fps=%d, bitrate=%d, gop=%d\n",
		width, height, fps, bitrate, gop);

	img_format = IMG_FMT_NV12;
	handle = vl_video_encoder_init(codec_id, width, height, fps, bitrate, gop);
	fprintf(stderr, "handle = %ld\n", handle);

	// Start streaming
	int bufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	io = ioctl(captureDev, VIDIOC_STREAMON, &bufferType);
	if (io < 0)
	{
		throw Exception("VIDIOC_STREAMON failed.");
	}

	int bufferSize = format.fmt.pix.width * format.fmt.pix.height * 4;
	unsigned char* srcBuf = new unsigned char[bufferSize];
	encodeInBuffer = srcBuf;
	fprintf(stderr, "Source Buffer Size = %d\n", bufferSize);

	encodeBitstreamBufferLength = bufferSize;
	encodeBitstreamBuffer = new unsigned char[bufferSize];

	bool isFirstFrame = true;
	int frames = 0;
	float totalTime = 0;
	sw.Start(); //ResetTime();
	
	timer.Callback = EncodeFrame;
	timer.SetInterval(1.0 / fps);

	while (true)
	{
		// get buffer
		v4l2_buffer buffer = { 0 };
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;

		io = ioctl(captureDev, VIDIOC_DQBUF, &buffer);
		if (io < 0)
		{
			throw Exception("VIDIOC_DQBUF failed.");
		}

		//Yuyv
		if (isFirstFrame)
		{
			frameRate = timer.Interval();
			timer.Start();
			isFirstFrame = false;
		}

		encodeMutex.Lock();

		// Process frame
		unsigned short* data = (unsigned short*)bufferMappings[buffer.index].Start;

		// convert YUYV to NV12
		int srcStride = format.fmt.pix.width;
		int dstStride = format.fmt.pix.width;
		int dstVUOffset = format.fmt.pix.width * format.fmt.pix.height;

		for (int y = 0; y < format.fmt.pix.height; ++y)
		{
			for (int x = 0; x < format.fmt.pix.width; x += 2)
			{
				int srcIndex = y * srcStride + x;
				//unsigned char l = data[srcIndex];
				unsigned short yu = data[srcIndex];
				unsigned short yv = data[srcIndex + 1];

				int dstIndex = (y * dstStride) + (x);
				srcBuf[dstIndex] = yu & 0xff;
				srcBuf[dstIndex + 1] = yv & 0xff;

				if (y % 2 == 0)
				{
					int dstVUIndex = (y >> 1) * dstStride + (x);
					if (img_format == IMG_FMT_NV12) {
						srcBuf[dstVUOffset + dstVUIndex] = yu >> 8;
						srcBuf[dstVUOffset + dstVUIndex + 1] = yv >> 8;
					} else { // IMG_FMT_NV21
						srcBuf[dstVUOffset + dstVUIndex] = yv >> 8;
						srcBuf[dstVUOffset + dstVUIndex + 1] = yu >> 8;
					}
				}
			}
		}
		encodeMutex.Unlock();

		// return buffer
		io = ioctl(captureDev, VIDIOC_QBUF, &buffer);
		if (io < 0)
		{
			throw Exception("VIDIOC_QBUF failed.");
		}

		// Measure FPS
		++frames;
		totalTime += (float)sw.Elapsed(); //GetTime();

		sw.Reset();

		if (totalTime >= 1.0f)
		{
			int fps = (int)(frames / totalTime);
			fprintf(stderr, "FPS: %i\n", fps);

			frames = 0;
			totalTime = 0;
		}
	}
	timer.Stop();
	close(fdOut);
	close(captureDev);

	delete encodeInBuffer;
	delete encodeBitstreamBuffer;

	vl_video_encoder_destroy(handle);

	return 0;
}
