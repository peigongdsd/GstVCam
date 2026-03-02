#include "pch.h"
#include "Tools.h"
#include "GstPipelineSource.h"

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

namespace
{
	std::once_flag g_gstInitOnce;
	HRESULT g_gstInitHr = E_FAIL;
	constexpr ULONGLONG kNoSampleLogIntervalMs = 2000;
	constexpr ULONGLONG kFallbackLogIntervalMs = 2000;

	std::wstring ReadEnvVar(PCWSTR name)
	{
		if (!name || !*name)
		{
			return {};
		}

		const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
		if (required == 0)
		{
			return {};
		}

		std::wstring value;
		value.resize(required - 1);
		if (GetEnvironmentVariableW(name, value.data(), required) == 0)
		{
			return {};
		}
		return value;
	}

	std::wstring TruncateForLog(const std::wstring& text, size_t maxChars)
	{
		if (text.size() <= maxChars)
		{
			return text;
		}
		return text.substr(0, maxChars) + L"...";
	}

	void LogProcessContext()
	{
		wchar_t exePath[MAX_PATH]{};
		DWORD exeLen = GetModuleFileNameW(nullptr, exePath, _countof(exePath));
		if (exeLen == 0 || exeLen >= _countof(exePath))
		{
			StringCchCopyW(exePath, _countof(exePath), L"<unknown>");
		}

		wchar_t userName[256]{};
		DWORD userLen = _countof(userName);
		if (!GetUserNameW(userName, &userLen) || userName[0] == L'\0')
		{
			StringCchCopyW(userName, _countof(userName), L"<unknown>");
		}

		WINTRACE(
			L"GStreamer runtime context pid:%u user:%s exe:%s",
			GetCurrentProcessId(),
			userName,
			exePath);
	}

	void LogGstEnvironment()
	{
		const auto pluginPath10 = ReadEnvVar(L"GST_PLUGIN_PATH_1_0");
		const auto pluginPath = ReadEnvVar(L"GST_PLUGIN_PATH");
		const auto pluginSysPath10 = ReadEnvVar(L"GST_PLUGIN_SYSTEM_PATH_1_0");
		const auto pluginSysPath = ReadEnvVar(L"GST_PLUGIN_SYSTEM_PATH");
		const auto gstRegistry = ReadEnvVar(L"GST_REGISTRY");
		const auto pathValue = ReadEnvVar(L"PATH");

		WINTRACE(L"GST_PLUGIN_PATH_1_0=%s", pluginPath10.empty() ? L"<unset>" : TruncateForLog(pluginPath10, 1024).c_str());
		WINTRACE(L"GST_PLUGIN_PATH=%s", pluginPath.empty() ? L"<unset>" : TruncateForLog(pluginPath, 1024).c_str());
		WINTRACE(L"GST_PLUGIN_SYSTEM_PATH_1_0=%s", pluginSysPath10.empty() ? L"<unset>" : TruncateForLog(pluginSysPath10, 1024).c_str());
		WINTRACE(L"GST_PLUGIN_SYSTEM_PATH=%s", pluginSysPath.empty() ? L"<unset>" : TruncateForLog(pluginSysPath, 1024).c_str());
		WINTRACE(L"GST_REGISTRY=%s", gstRegistry.empty() ? L"<unset>" : TruncateForLog(gstRegistry, 1024).c_str());
		WINTRACE(L"PATH length:%zu head:%s", pathValue.size(), pathValue.empty() ? L"<unset>" : TruncateForLog(pathValue, 512).c_str());
	}

	void LogElementFactoryAvailability(const char* elementName)
	{
		if (!elementName || !*elementName)
		{
			return;
		}

		auto registry = gst_registry_get();
		if (!registry)
		{
			WINTRACE(L"GStreamer registry unavailable while probing '%S'", elementName);
			return;
		}

		GstPluginFeature* feature = gst_registry_find_feature(registry, elementName, GST_TYPE_ELEMENT_FACTORY);
		if (!feature)
		{
			WINTRACE(L"GStreamer element factory '%S' not found in registry", elementName);
			return;
		}

		const gchar* pluginName = gst_plugin_feature_get_plugin_name(feature);
		const guint rank = gst_plugin_feature_get_rank(feature);
		WINTRACE(
			L"GStreamer element factory '%S' found plugin:%S rank:%u",
			elementName,
			pluginName ? pluginName : "",
			rank);
		gst_object_unref(feature);
	}

	std::vector<std::wstring> GetShm2PluginCandidates()
	{
		std::vector<std::wstring> candidates;
		const auto explicitPath = ReadEnvVar(L"GSTVCAM_SHM2_PLUGIN_PATH");
		if (!explicitPath.empty())
		{
			candidates.push_back(explicitPath);
		}

		candidates.push_back(L"C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\\lib\\gstreamer-1.0\\gstshm2.dll");
		return candidates;
	}

	void ProbePluginLoadByFilePath(const std::wstring& pluginPath)
	{
		if (pluginPath.empty())
		{
			return;
		}

		const DWORD attrs = GetFileAttributesW(pluginPath.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
		{
			WINTRACE(L"Plugin probe path not found: %s", pluginPath.c_str());
			return;
		}

		GError* error = nullptr;
		const auto pluginPathA = to_string(pluginPath);
		GstPlugin* plugin = gst_plugin_load_file(pluginPathA.c_str(), &error);
		if (!plugin)
		{
			if (error)
			{
				WINTRACE(
					L"Plugin load failed path:%s domain:%u code:%d message:%s",
					pluginPath.c_str(),
					error->domain,
					error->code,
					to_wstring(error->message).c_str());
				g_clear_error(&error);
			}
			else
			{
				WINTRACE(L"Plugin load failed path:%s with unknown error", pluginPath.c_str());
			}
			return;
		}

		const gchar* name = gst_plugin_get_name(plugin);
		const gchar* desc = gst_plugin_get_description(plugin);
		const gchar* version = gst_plugin_get_version(plugin);
		WINTRACE(
			L"Plugin load success path:%s name:%S version:%S desc:%S",
			pluginPath.c_str(),
			name ? name : "",
			version ? version : "",
			desc ? desc : "");
		gst_object_unref(plugin);
	}

	void ProbeShm2PluginLoad()
	{
		const auto candidates = GetShm2PluginCandidates();
		for (const auto& path : candidates)
		{
			ProbePluginLoadByFilePath(path);
		}
	}

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
			LogProcessContext();
			LogGstEnvironment();
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

			guint major = 0;
			guint minor = 0;
			guint micro = 0;
			guint nano = 0;
			gst_version(&major, &minor, &micro, &nano);
			WINTRACE(
				L"GStreamer initialized version:%u.%u.%u.%u",
				major,
				minor,
				micro,
				nano);
			ProbeShm2PluginLoad();
			LogElementFactoryAvailability("shm2src");
			LogElementFactoryAvailability("shmsrc");
			LogElementFactoryAvailability("appsink");
			g_gstInitHr = S_OK;
		});

	return g_gstInitHr;
}

HRESULT GstPipelineSource::Start(const VCamPipelineConfig& config)
{
	std::lock_guard<std::mutex> lock(_stateLock);
	RETURN_HR_IF(E_INVALIDARG, !config.width || !config.height || !config.fpsNumerator || !config.fpsDenominator);
	RETURN_IF_FAILED(EnsureGStreamerInitialized());
	if (_running.load())
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
	GstElement* pipeline = gst_parse_launch(pipelineA.c_str(), &parseError);
	if (parseError)
	{
		WINTRACE(
			L"GStreamer parse warning/error detail domain:%u code:%d message:%s",
			parseError->domain,
			parseError->code,
			to_wstring(parseError->message).c_str());
		LogElementFactoryAvailability("shm2src");
		LogElementFactoryAvailability("shmsrc");
		g_clear_error(&parseError);
	}
	RETURN_HR_IF_NULL_MSG(E_INVALIDARG, pipeline, "Invalid GStreamer pipeline");

	GstElement* appSinkElement = gst_bin_get_by_name(GST_BIN(pipeline), "vcamsink");
	if (!appSinkElement)
	{
		gst_object_unref(pipeline);
		RETURN_HR_MSG(E_FAIL, "Pipeline must expose an appsink named 'vcamsink'");
	}

	auto appSink = GST_APP_SINK(appSinkElement);
	g_object_set(
		G_OBJECT(appSinkElement),
		"emit-signals", FALSE,
		"sync", TRUE,
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
	if (caps)
	{
		gst_app_sink_set_caps(appSink, caps);
		gst_caps_unref(caps);
	}

	GstBus* bus = gst_element_get_bus(pipeline);
	if (!bus)
	{
		gst_object_unref(appSinkElement);
		gst_object_unref(pipeline);
		RETURN_HR(E_FAIL);
	}

	_pipeline = pipeline;
	_appSinkElement = appSinkElement;
	_appSink = appSink;
	_bus = bus;
	WINTRACE(L"GstPipelineSource::Start bus:%p appsink:%p", _bus, _appSinkElement);

	{
		// Keep only the latest sample to minimize latency while clients switch rapidly.
		std::lock_guard<std::mutex> frameLock(_frameLock);
		if (_latestSample)
		{
			gst_sample_unref(_latestSample);
			_latestSample = nullptr;
		}
		_hasFrame = false;
		_formatMismatchLogged.store(false);
		_firstFrameLogged.store(false);
		_firstCopyLogged.store(false);
	}

	const auto now = GetTickCount64();
	_lastNoSampleLogTick.store(now);
	_lastFallbackLogTick.store(now);
	_running.store(true);
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

	_running.store(false);
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
	{
		std::lock_guard<std::mutex> lock(_frameLock);
		if (_latestSample)
		{
			gst_sample_unref(_latestSample);
			_latestSample = nullptr;
		}
		_hasFrame = false;
		_latestFrameId = 0;
	}

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
	if (_pipeline && _running.load())
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

	while (_running.load())
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
			if (now - _lastNoSampleLogTick.load() >= kNoSampleLogIntervalMs)
			{
				_lastNoSampleLogTick.store(now);
				WINTRACE(L"No sample pulled from appsink for %llu ms", kNoSampleLogIntervalMs);
			}
			continue;
		}

		_lastNoSampleLogTick.store(GetTickCount64());
		const auto hr = StoreSample(sample);
		if (FAILED(hr) && !_formatMismatchLogged.exchange(true))
		{
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

	const auto yStride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
	const auto uvStride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 1);
	if (!_firstFrameLogged.exchange(true))
	{
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

	{
		std::lock_guard<std::mutex> lock(_frameLock);
		if (_latestSample)
		{
			gst_sample_unref(_latestSample);
		}
		_latestSample = gst_sample_ref(sample);
		_hasFrame = true;
		_latestFrameId++;
	}
	return S_OK;
}

bool GstPipelineSource::HasNewFrameSince(uint64_t lastDeliveredFrameId, uint64_t* outLatestFrameId)
{
	if (outLatestFrameId)
	{
		*outLatestFrameId = 0;
	}

	std::lock_guard<std::mutex> lock(_frameLock);
	if (!_hasFrame || !_latestSample)
	{
		return false;
	}
	if (_latestFrameId <= lastDeliveredFrameId)
	{
		return false;
	}
	if (outLatestFrameId)
	{
		*outLatestFrameId = _latestFrameId;
	}
	return true;
}

HRESULT GstPipelineSource::CopyLatestFrameTo(BYTE* destination, LONG destinationStride, DWORD destinationLength, uint64_t minimumFrameIdExclusive, uint64_t* outCopiedFrameId)
{
	RETURN_HR_IF_NULL(E_POINTER, destination);
	RETURN_HR_IF(E_INVALIDARG, destinationStride <= 0);
	RETURN_HR_IF(E_INVALIDARG, destinationStride < static_cast<LONG>(_config.width));
	RETURN_HR_IF_NULL(E_POINTER, outCopiedFrameId);
	*outCopiedFrameId = 0;

	const auto requiredLength = static_cast<size_t>(destinationStride) * _config.height * 3 / 2;
	RETURN_HR_IF(E_BOUNDS, destinationLength < requiredLength);

	GstSample* sample = nullptr;
	bool hasFrame = false;
	uint64_t frameId = 0;
	{
		std::lock_guard<std::mutex> lock(_frameLock);
		hasFrame = _hasFrame;
		if (_hasFrame && _latestSample)
		{
			frameId = _latestFrameId;
			sample = gst_sample_ref(_latestSample);
		}
	}

	if (!sample || frameId <= minimumFrameIdExclusive)
	{
		const auto now = GetTickCount64();
		if (now - _lastFallbackLogTick.load() >= kFallbackLogIntervalMs)
		{
			_lastFallbackLogTick.store(now);
			WINTRACE(
				L"No new frame available hasFrame:%u sample:%p frameId:%llu lastDelivered:%llu",
				hasFrame ? 1 : 0,
				sample,
				frameId,
				minimumFrameIdExclusive);
		}
		if (sample)
		{
			gst_sample_unref(sample);
		}
		return S_FALSE;
	}

	if (!_firstCopyLogged.exchange(true))
	{
		WINTRACE(L"First frame copy to MF buffer stride:%ld length:%u", destinationStride, destinationLength);
	}

	HRESULT hr = S_OK;
	bool frameMapped = false;
	GstVideoFrame frame{};
	const BYTE* ySrc = nullptr;
	const BYTE* uvSrc = nullptr;
	int yStride = 0;
	int uvStride = 0;
	BYTE* uvDst = nullptr;
	GstCaps* caps = gst_sample_get_caps(sample);
	GstBuffer* buffer = gst_sample_get_buffer(sample);
	if (!caps || !buffer)
	{
		hr = E_FAIL;
		goto Cleanup;
	}

	GstVideoInfo info;
	if (!gst_video_info_from_caps(&info, caps))
	{
		hr = E_FAIL;
		goto Cleanup;
	}
	if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12)
	{
		hr = MF_E_INVALIDMEDIATYPE;
		goto Cleanup;
	}
	if (GST_VIDEO_INFO_WIDTH(&info) != _config.width || GST_VIDEO_INFO_HEIGHT(&info) != _config.height)
	{
		hr = MF_E_INVALIDMEDIATYPE;
		goto Cleanup;
	}

	if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
	{
		hr = E_FAIL;
		goto Cleanup;
	}
	frameMapped = true;

	ySrc = static_cast<const BYTE*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
	uvSrc = static_cast<const BYTE*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
	yStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
	uvStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);

	for (UINT row = 0; row < _config.height; row++)
	{
		memcpy(
			destination + static_cast<size_t>(row) * destinationStride,
			ySrc + static_cast<ptrdiff_t>(row) * yStride,
			_config.width);
	}

	uvDst = destination + static_cast<size_t>(destinationStride) * _config.height;
	for (UINT row = 0; row < _config.height / 2; row++)
	{
		memcpy(
			uvDst + static_cast<size_t>(row) * destinationStride,
			uvSrc + static_cast<ptrdiff_t>(row) * uvStride,
			_config.width);
	}

Cleanup:
	if (frameMapped)
	{
		gst_video_frame_unmap(&frame);
	}
	gst_sample_unref(sample);
	if (SUCCEEDED(hr))
	{
		*outCopiedFrameId = frameId;
	}
	return hr;
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
