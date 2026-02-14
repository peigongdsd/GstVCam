#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "GstPipelineSource.h"
#include "MediaStream.h"
#include "MediaSource.h"

HRESULT MediaStream::Initialize(IMFMediaSource* source, int index, const VCamPipelineConfig& config)
{
	RETURN_HR_IF_NULL(E_POINTER, source);
	_source = source;
	_index = index;
	_config = config;

	RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, index));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

	auto types = wil::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(1);

	wil::com_ptr_nothrow<IMFMediaType> nv12Type;
	RETURN_IF_FAILED(MFCreateMediaType(&nv12Type));
	nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	nv12Type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	nv12Type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
	MFSetAttributeSize(nv12Type.get(), MF_MT_FRAME_SIZE, _config.width, _config.height);
	nv12Type->SetUINT32(MF_MT_DEFAULT_STRIDE, _config.width);
	MFSetAttributeRatio(nv12Type.get(), MF_MT_FRAME_RATE, _config.fpsNumerator, _config.fpsDenominator);

	auto bitrate = static_cast<uint32_t>((static_cast<double>(_config.width) * _config.height * 3.0 / 2.0) * 8.0 * _config.fpsNumerator / _config.fpsDenominator);
	nv12Type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
	MFSetAttributeRatio(nv12Type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	types[0] = nv12Type.detach();

	RETURN_IF_FAILED_MSG(MFCreateStreamDescriptor(_index, (DWORD)types.size(), types.get(), &_descriptor), "MFCreateStreamDescriptor failed");

	wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
	RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
	TraceMFAttributes(handler.get(), L"MediaTypeHandler");
	RETURN_IF_FAILED(handler->SetCurrentMediaType(types[0]));
	_frameDuration = static_cast<LONGLONG>(10000000.0 * _config.fpsDenominator / _config.fpsNumerator);

	return S_OK;
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);
	if (_state == MF_STREAM_STATE_RUNNING)
	{
		return S_OK;
	}
	WINTRACE(
		L"MediaStream::Start config width:%u height:%u fps:%u/%u",
		_config.width,
		_config.height,
		_config.fpsNumerator,
		_config.fpsDenominator);

	wil::com_ptr_nothrow<IMFMediaType> currentType;
	if (!type)
	{
		wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
		RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
		RETURN_IF_FAILED(handler->GetCurrentMediaType(&currentType));
		type = currentType.get();
	}

	if (type)
	{
		RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &_format));
		WINTRACE(L"MediaStream::Start format: %s", GUID_ToStringW(_format).c_str());
	}

	RETURN_HR_IF_MSG(MF_E_INVALIDMEDIATYPE, _format != MFVideoFormat_NV12, "Only NV12 stream format is supported");
	RETURN_IF_FAILED(_pipelineSource.Start(_config));

	RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(10, type));
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_RUNNING;
	return S_OK;
}

HRESULT MediaStream::Stop()
{
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);
	if (_state == MF_STREAM_STATE_STOPPED)
	{
		return S_OK;
	}

	RETURN_IF_FAILED(_allocator->UninitializeSampleAllocator());
	_pipelineSource.Stop();
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_STOPPED;
	return S_OK;
}

MFSampleAllocatorUsage MediaStream::GetAllocatorUsage()
{
	return MFSampleAllocatorUsage_UsesProvidedAllocator;
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
	RETURN_HR_IF_NULL(E_POINTER, allocator);
	_allocator.reset();
	RETURN_HR(allocator->QueryInterface(&_allocator));
}

HRESULT MediaStream::SetD3DManager(IUnknown* manager)
{
	RETURN_HR_IF_NULL(E_POINTER, manager);
	// This stream writes CPU NV12 bytes from GStreamer appsink.
	// Keep this as a no-op so allocator remains CPU-backed.
	return S_OK;
}

void MediaStream::Shutdown()
{
	winrt::slim_lock_guard lock(_lock);
	_pipelineSource.Stop();

	if (_queue)
	{
		LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
		_queue.reset();
	}

	_allocator.reset();
	_descriptor.reset();
	_source.reset();
	_attributes.reset();
	_state = MF_STREAM_STATE_STOPPED;
}

// IMFMediaEventGenerator
STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	//WINTRACE(L"MediaSource::BeginGetEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	//WINTRACE(L"MediaStream::EndGetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	WINTRACE(L"MediaStream::GetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	WINTRACE(L"MediaStream::QueueEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

// IMFMediaStream
STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
	WINTRACE(L"MediaSource::GetMediaSource");
	RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
	*ppMediaSource = nullptr;
	RETURN_HR_IF(MF_E_SHUTDOWN, !_source);

	RETURN_IF_FAILED(_source.copy_to(ppMediaSource));
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
	WINTRACE(L"MediaStream::GetStreamDescriptor");
	RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
	*ppStreamDescriptor = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);

	RETURN_IF_FAILED(_descriptor.copy_to(ppStreamDescriptor));
	return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
	//WINTRACE(L"MediaStream::RequestSample pToken:%p", pToken);
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_allocator || !_queue);
	RETURN_HR_IF(MF_E_INVALIDREQUEST, _state != MF_STREAM_STATE_RUNNING);

	wil::com_ptr_nothrow<IMFSample> sample;
	RETURN_IF_FAILED(_allocator->AllocateSample(&sample));
	RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
	RETURN_IF_FAILED(sample->SetSampleDuration(_frameDuration));

	wil::com_ptr_nothrow<IMFMediaBuffer> mediaBuffer;
	RETURN_IF_FAILED(sample->GetBufferByIndex(0, &mediaBuffer));
	wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
	RETURN_IF_FAILED(mediaBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D)));

	BYTE* scanline = nullptr;
	BYTE* start = nullptr;
	LONG pitch = 0;
	DWORD length = 0;
	RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &length));
	const auto copyHr = _pipelineSource.CopyLatestFrameTo(scanline, pitch, length);
	buffer2D->Unlock2D();
	RETURN_IF_FAILED(copyHr);
	_requestCount++;
	const auto now = GetTickCount64();
	if (_requestCount == 1 || now - _lastRequestTraceTick >= 2000)
	{
		_lastRequestTraceTick = now;
		WINTRACE(
			L"MediaStream::RequestSample count:%u pitch:%ld length:%u",
			_requestCount,
			pitch,
			length);
	}

	if (pToken)
	{
		RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, pToken));
	}
	RETURN_IF_FAILED(_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get()));
	return S_OK;
}

// IMFMediaStream2
STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
	MF_STREAM_STATE currentState = MF_STREAM_STATE_STOPPED;
	RETURN_IF_FAILED(GetStreamState(&currentState));
	WINTRACE(L"MediaStream::SetStreamState current:%u value:%u", currentState, value);
	if (currentState == value)
		return S_OK;
	switch (value)
	{
	case MF_STREAM_STATE_PAUSED:
		if (currentState != MF_STREAM_STATE_RUNNING)
			RETURN_HR(MF_E_INVALID_STATE_TRANSITION);

		{
			winrt::slim_lock_guard lock(_lock);
			_state = value;
		}
		break;

	case MF_STREAM_STATE_RUNNING:
		RETURN_IF_FAILED(Start(nullptr));
		break;

	case MF_STREAM_STATE_STOPPED:
		RETURN_IF_FAILED(Stop());
		break;

	default:
		RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
		break;
	}
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
	RETURN_HR_IF_NULL(E_POINTER, value);
	winrt::slim_lock_guard lock(_lock);
	WINTRACE(L"MediaStream::GetStreamState state:%u", _state);
	*value = _state;
	return S_OK;
}

// IKsControl
STDMETHODIMP_(NTSTATUS) MediaStream::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsProperty len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsProperty prop:%s", PKSIDENTIFIER_ToString(property, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsMethod len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsMethod method:%s", PKSIDENTIFIER_ToString(method, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsEvent evt:%p len:%u data:%p dataLength:%u", evt, length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsEvent event:%s", PKSIDENTIFIER_ToString(evt, length).c_str());
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
