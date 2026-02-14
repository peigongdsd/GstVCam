#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
	std::mutex _stateLock;
	std::thread _pullThread;
	std::mutex _frameLock;
	std::vector<BYTE> _latestFrame;
	bool _hasFrame = false;
	bool _formatMismatchLogged = false;
	bool _firstFrameLogged = false;
	bool _firstCopyLogged = false;
	ULONGLONG _lastNoSampleLogTick = 0;
	ULONGLONG _lastFallbackLogTick = 0;

	VCamPipelineConfig _config;
	GstElement* _pipeline = nullptr;
	GstElement* _appSinkElement = nullptr;
	GstAppSink* _appSink = nullptr;
	GstBus* _bus = nullptr;
};
