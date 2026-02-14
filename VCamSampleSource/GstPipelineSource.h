#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

typedef struct _GstElement GstElement;
typedef struct _GstAppSink GstAppSink;
typedef struct _GstSample GstSample;
typedef struct _GstBus GstBus;

struct VCamPipelineConfig
{
	std::wstring pipeline;
	UINT width = 1280;
	UINT height = 960;
	UINT fpsNumerator = 30;
	UINT fpsDenominator = 1;
};

class GstPipelineSource
{
public:
	GstPipelineSource() = default;
	~GstPipelineSource();

	HRESULT Start(const VCamPipelineConfig& config);
	void Stop();
	HRESULT CopyLatestFrameTo(BYTE* destination, LONG destinationStride, DWORD destinationLength);

private:
	HRESULT EnsureGStreamerInitialized();
	HRESULT StoreSample(GstSample* sample);
	void PullLoop();
	void ResetPipelineObjects();
	void DrainBusMessages();

private:
	std::atomic<bool> _running = false;
	// Protects start/stop transitions and ownership of GStreamer objects.
	std::mutex _stateLock;
	std::thread _pullThread;
	// Protects _latestSample/_hasFrame shared between pull and MF request threads.
	std::mutex _frameLock;
	GstSample* _latestSample = nullptr;
	bool _hasFrame = false;
	std::atomic<bool> _formatMismatchLogged = false;
	std::atomic<bool> _firstFrameLogged = false;
	std::atomic<bool> _firstCopyLogged = false;
	std::atomic<ULONGLONG> _lastNoSampleLogTick = 0;
	std::atomic<ULONGLONG> _lastFallbackLogTick = 0;

	VCamPipelineConfig _config;
	GstElement* _pipeline = nullptr;
	GstElement* _appSinkElement = nullptr;
	GstAppSink* _appSink = nullptr;
	GstBus* _bus = nullptr;
};
