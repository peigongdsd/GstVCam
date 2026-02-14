#include "pch.h"
#include "Tools.h"
#include "GstPipelineSource.h"

#include <algorithm>
#include <cwctype>
#include <mutex>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

namespace
{
	std::once_flag g_gstInitOnce;
	HRESULT g_gstInitHr = E_FAIL;
	constexpr ULONGLONG kNoSampleLogIntervalMs = 2000;
	constexpr ULONGLONG kFallbackLogIntervalMs = 2000;

	bool ContainsCaseInsensitive(const std::wstring& value, const std::wstring& token)
	{
		std::wstring lowerValue = value;
		std::wstring lowerToken = token;
		std::transform(lowerValue.begin(), lowerValue.end(), lowerValue.begin(), towlower);
		std::transform(lowerToken.begin(), lowerToken.end(), lowerToken.begin(), towlower);
		return lowerValue.find(lowerToken) != std::wstring::npos;
	}

	std::wstring BuildDefaultPipeline(const VCamPipelineConfig& config)
	{
		return std::format(
			L"videotestsrc is-live=true pattern=smpte ! video/x-raw,format=NV12,width={},height={},framerate={}/{} ! appsink name=vcamsink",
			config.width,
			config.height,
			config.fpsNumerator,
			config.fpsDenominator);
	}
}

GstPipelineSource::~GstPipelineSource()
{
	Stop();
}

HRESULT GstPipelineSource::EnsureGStreamerInitialized()
{
	std::call_once(g_gstInitOnce, []()
		{
			GError* error = nullptr;
			if (!gst_init_check(nullptr, nullptr, &error))
			{
				if (error)
				{
					WINTRACE(L"GStreamer initialization failed: %s", to_wstring(error->message).c_str());
					g_clear_error(&error);
				}
				g_gstInitHr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
				return;
			}

			g_gstInitHr = S_OK;
		});

	return g_gstInitHr;
}

HRESULT GstPipelineSource::Start(const VCamPipelineConfig& config)
{
	std::lock_guard<std::mutex> lock(_stateLock);
	RETURN_HR_IF(E_INVALIDARG, !config.width || !config.height || !config.fpsNumerator || !config.fpsDenominator);
	RETURN_IF_FAILED(EnsureGStreamerInitialized());
	if (_running)
	{
		return S_OK;
	}

	_config = config;
	if (_config.pipeline.empty())
	{
		_config.pipeline = BuildDefaultPipeline(_config);
	}
	else if (!ContainsCaseInsensitive(_config.pipeline, L"appsink"))
	{
		_config.pipeline += L" ! appsink name=vcamsink";
	}
	WINTRACE(
		L"GstPipelineSource::Start width:%u height:%u fps:%u/%u pipeline:%s",
		_config.width,
		_config.height,
		_config.fpsNumerator,
		_config.fpsDenominator,
		_config.pipeline.c_str());

	auto pipelineA = to_string(_config.pipeline);
	GError* parseError = nullptr;
	_pipeline = gst_parse_launch(pipelineA.c_str(), &parseError);
	if (parseError)
	{
		WINTRACE(L"GStreamer parse warning/error detail: %s", to_wstring(parseError->message).c_str());
		g_clear_error(&parseError);
	}
	if (!_pipeline)
	{
		RETURN_HR_MSG(E_INVALIDARG, "Invalid GStreamer pipeline");
	}

	_appSinkElement = gst_bin_get_by_name(GST_BIN(_pipeline), "vcamsink");
	RETURN_HR_IF_NULL_MSG(E_FAIL, _appSinkElement, "Pipeline must expose an appsink named 'vcamsink'");
	_appSink = GST_APP_SINK(_appSinkElement);

	g_object_set(
		G_OBJECT(_appSinkElement),
		"emit-signals", FALSE,
		"sync", FALSE,
		"max-buffers", 2u,
		"drop", TRUE,
		nullptr);

	GstCaps* caps = gst_caps_new_simple(
		"video/x-raw",
		"format", G_TYPE_STRING, "NV12",
		"width", G_TYPE_INT, static_cast<int>(_config.width),
		"height", G_TYPE_INT, static_cast<int>(_config.height),
		"framerate", GST_TYPE_FRACTION, static_cast<int>(_config.fpsNumerator), static_cast<int>(_config.fpsDenominator),
		nullptr);
	gst_app_sink_set_caps(_appSink, caps);
	gst_caps_unref(caps);

	_bus = gst_element_get_bus(_pipeline);
	WINTRACE(L"GstPipelineSource::Start bus:%p appsink:%p", _bus, _appSinkElement);

	{
		std::lock_guard<std::mutex> frameLock(_frameLock);
		_latestFrame.clear();
		_hasFrame = false;
		_formatMismatchLogged = false;
		_firstFrameLogged = false;
		_firstCopyLogged = false;
	}
	_lastNoSampleLogTick = GetTickCount64();
	_lastFallbackLogTick = GetTickCount64();

	_running = true;
	_pullThread = std::thread(&GstPipelineSource::PullLoop, this);
	WINTRACE(L"GStreamer pipeline started");
	return S_OK;
}

void GstPipelineSource::Stop()
{
	std::lock_guard<std::mutex> lock(_stateLock);
	if (!_pipeline && !_pullThread.joinable())
	{
		return;
	}

	_running = false;
	if (_pipeline)
	{
		const auto stateResult = gst_element_set_state(_pipeline, GST_STATE_NULL);
		WINTRACE(L"gst_element_set_state(NULL) => %d", stateResult);
	}

	if (_pullThread.joinable())
	{
		_pullThread.join();
	}

	ResetPipelineObjects();
	WINTRACE(L"GStreamer pipeline stopped");
}

void GstPipelineSource::ResetPipelineObjects()
{
	if (_bus)
	{
		gst_object_unref(_bus);
		_bus = nullptr;
	}

	if (_appSinkElement)
	{
		gst_object_unref(_appSinkElement);
		_appSinkElement = nullptr;
		_appSink = nullptr;
	}

	if (_pipeline)
	{
		gst_object_unref(_pipeline);
		_pipeline = nullptr;
	}
}

void GstPipelineSource::PullLoop()
{
	WINTRACE(L"GstPipelineSource::PullLoop enter");
	if (_pipeline)
	{
		WINTRACE(L"About to call gst_element_set_state(PLAYING)");
		const auto stateResult = gst_element_set_state(_pipeline, GST_STATE_PLAYING);
		WINTRACE(L"gst_element_set_state(PLAYING) => %d", stateResult);
		if (stateResult == GST_STATE_CHANGE_FAILURE)
		{
			WINTRACE(L"Failed to start GStreamer pipeline in pull thread");
		}
		else
		{
			GstState currentState = GST_STATE_NULL;
			GstState pendingState = GST_STATE_VOID_PENDING;
			const auto waitResult = gst_element_get_state(_pipeline, &currentState, &pendingState, 2000 * GST_MSECOND);
			WINTRACE(
				L"gst_element_get_state => %d current:%S pending:%S",
				waitResult,
				gst_element_state_get_name(currentState),
				gst_element_state_get_name(pendingState));
		}
	}

	while (_running)
	{
		DrainBusMessages();

		if (!_appSink)
		{
			Sleep(10);
			continue;
		}

		GstSample* sample = gst_app_sink_try_pull_sample(_appSink, 200 * GST_MSECOND);
		if (!sample)
		{
			const auto now = GetTickCount64();
			if (now - _lastNoSampleLogTick >= kNoSampleLogIntervalMs)
			{
				_lastNoSampleLogTick = now;
				WINTRACE(L"No sample pulled from appsink for %llu ms", kNoSampleLogIntervalMs);
			}
			continue;
		}
		_lastNoSampleLogTick = GetTickCount64();

		const auto hr = StoreSample(sample);
		if (FAILED(hr) && !_formatMismatchLogged)
		{
			_formatMismatchLogged = true;
			WINTRACE(L"Could not consume sample from appsink, hr:0x%08X", hr);
		}

		gst_sample_unref(sample);
	}

	DrainBusMessages();
	WINTRACE(L"GstPipelineSource::PullLoop exit");
}

HRESULT GstPipelineSource::StoreSample(GstSample* sample)
{
	RETURN_HR_IF_NULL(E_POINTER, sample);

	GstCaps* caps = gst_sample_get_caps(sample);
	GstBuffer* buffer = gst_sample_get_buffer(sample);
	RETURN_HR_IF_NULL(E_FAIL, caps);
	RETURN_HR_IF_NULL(E_FAIL, buffer);

	GstVideoInfo info;
	RETURN_HR_IF(E_FAIL, !gst_video_info_from_caps(&info, caps));
	if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12)
	{
		WINTRACE(L"Unexpected sink format. Expected NV12, got:%d", GST_VIDEO_INFO_FORMAT(&info));
		RETURN_HR(MF_E_INVALIDMEDIATYPE);
	}
	if (GST_VIDEO_INFO_WIDTH(&info) != _config.width || GST_VIDEO_INFO_HEIGHT(&info) != _config.height)
	{
		WINTRACE(
			L"Unexpected sink dimensions. Expected %ux%u, got %ux%u",
			_config.width,
			_config.height,
			GST_VIDEO_INFO_WIDTH(&info),
			GST_VIDEO_INFO_HEIGHT(&info));
		RETURN_HR(MF_E_INVALIDMEDIATYPE);
	}

	GstVideoFrame frame;
	RETURN_HR_IF(E_FAIL, !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ));

	std::vector<BYTE> packedFrame;
	packedFrame.resize(static_cast<size_t>(_config.width) * _config.height * 3 / 2);

	BYTE* yDst = packedFrame.data();
	BYTE* uvDst = packedFrame.data() + static_cast<size_t>(_config.width) * _config.height;

	BYTE* ySrc = static_cast<BYTE*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
	BYTE* uvSrc = static_cast<BYTE*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
	const auto yStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
	const auto uvStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
	if (!_firstFrameLogged)
	{
		_firstFrameLogged = true;
		const auto capsStr = gst_caps_to_string(caps);
		WINTRACE(
			L"First sample received caps:%S yStride:%d uvStride:%d ySize:%u uvSize:%u",
			capsStr ? capsStr : "",
			yStride,
			uvStride,
			_config.width * _config.height,
			(_config.width * _config.height) / 2);
		if (capsStr)
		{
			g_free(capsStr);
		}
	}

	for (UINT row = 0; row < _config.height; row++)
	{
		memcpy(yDst + static_cast<size_t>(row) * _config.width, ySrc + static_cast<ptrdiff_t>(row) * yStride, _config.width);
	}

	for (UINT row = 0; row < _config.height / 2; row++)
	{
		memcpy(uvDst + static_cast<size_t>(row) * _config.width, uvSrc + static_cast<ptrdiff_t>(row) * uvStride, _config.width);
	}

	gst_video_frame_unmap(&frame);

	{
		std::lock_guard<std::mutex> lock(_frameLock);
		_latestFrame.swap(packedFrame);
		_hasFrame = true;
	}
	return S_OK;
}

HRESULT GstPipelineSource::CopyLatestFrameTo(BYTE* destination, LONG destinationStride, DWORD destinationLength)
{
	RETURN_HR_IF_NULL(E_POINTER, destination);
	RETURN_HR_IF(E_INVALIDARG, destinationStride <= 0);
	RETURN_HR_IF(E_INVALIDARG, destinationStride < static_cast<LONG>(_config.width));

	const auto requiredLength = static_cast<size_t>(destinationStride) * _config.height * 3 / 2;
	RETURN_HR_IF(E_BOUNDS, destinationLength < requiredLength);

	std::lock_guard<std::mutex> lock(_frameLock);
	if (!_hasFrame || _latestFrame.size() < static_cast<size_t>(_config.width) * _config.height * 3 / 2)
	{
		const auto now = GetTickCount64();
		if (now - _lastFallbackLogTick >= kFallbackLogIntervalMs)
		{
			_lastFallbackLogTick = now;
			WINTRACE(
				L"Using fallback black frame hasFrame:%u latestFrameBytes:%zu stride:%ld length:%u",
				_hasFrame ? 1 : 0,
				_latestFrame.size(),
				destinationStride,
				destinationLength);
		}

		// Black NV12 frame.
		for (UINT row = 0; row < _config.height; row++)
		{
			memset(destination + static_cast<size_t>(row) * destinationStride, 16, _config.width);
		}

		BYTE* uv = destination + static_cast<size_t>(destinationStride) * _config.height;
		for (UINT row = 0; row < _config.height / 2; row++)
		{
			memset(uv + static_cast<size_t>(row) * destinationStride, 128, _config.width);
		}
		return S_OK;
	}
	if (!_firstCopyLogged)
	{
		_firstCopyLogged = true;
		WINTRACE(L"First frame copy to MF buffer stride:%ld length:%u", destinationStride, destinationLength);
	}

	const BYTE* ySrc = _latestFrame.data();
	const BYTE* uvSrc = _latestFrame.data() + static_cast<size_t>(_config.width) * _config.height;
	for (UINT row = 0; row < _config.height; row++)
	{
		memcpy(
			destination + static_cast<size_t>(row) * destinationStride,
			ySrc + static_cast<size_t>(row) * _config.width,
			_config.width);
	}

	BYTE* uvDst = destination + static_cast<size_t>(destinationStride) * _config.height;
	for (UINT row = 0; row < _config.height / 2; row++)
	{
		memcpy(
			uvDst + static_cast<size_t>(row) * destinationStride,
			uvSrc + static_cast<size_t>(row) * _config.width,
			_config.width);
	}
	return S_OK;
}

void GstPipelineSource::DrainBusMessages()
{
	if (!_bus)
	{
		return;
	}

	while (auto message = gst_bus_pop(_bus))
	{
		const auto type = GST_MESSAGE_TYPE(message);
		switch (type)
		{
		case GST_MESSAGE_ERROR:
		{
			GError* error = nullptr;
			gchar* debug = nullptr;
			gst_message_parse_error(message, &error, &debug);
			const char* srcName = GST_OBJECT_NAME(message->src);
			WINTRACE(
				L"GStreamer bus ERROR src:%S message:%s debug:%s",
				srcName ? srcName : "",
				error ? to_wstring(error->message).c_str() : L"",
				debug ? to_wstring(debug).c_str() : L"");
			if (error)
			{
				g_clear_error(&error);
			}
			if (debug)
			{
				g_free(debug);
			}
			break;
		}
		case GST_MESSAGE_WARNING:
		{
			GError* error = nullptr;
			gchar* debug = nullptr;
			gst_message_parse_warning(message, &error, &debug);
			const char* srcName = GST_OBJECT_NAME(message->src);
			WINTRACE(
				L"GStreamer bus WARNING src:%S message:%s debug:%s",
				srcName ? srcName : "",
				error ? to_wstring(error->message).c_str() : L"",
				debug ? to_wstring(debug).c_str() : L"");
			if (error)
			{
				g_clear_error(&error);
			}
			if (debug)
			{
				g_free(debug);
			}
			break;
		}
		case GST_MESSAGE_EOS:
			WINTRACE(L"GStreamer bus EOS");
			break;
		case GST_MESSAGE_STATE_CHANGED:
			if (GST_MESSAGE_SRC(message) == GST_OBJECT(_pipeline))
			{
				GstState oldState = GST_STATE_NULL;
				GstState newState = GST_STATE_NULL;
				GstState pendingState = GST_STATE_VOID_PENDING;
				gst_message_parse_state_changed(message, &oldState, &newState, &pendingState);
				WINTRACE(
					L"GStreamer state changed %S -> %S pending:%S",
					gst_element_state_get_name(oldState),
					gst_element_state_get_name(newState),
					gst_element_state_get_name(pendingState));
			}
			break;
		default:
			break;
		}

		gst_message_unref(message);
	}
}
