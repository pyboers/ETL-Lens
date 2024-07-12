#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <tchar.h>
#include <map>

#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif

#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

#include "imgui/imgui_internal.h"
#include <D3DWrappers/CommandAllocatorWrapper.h>
#include <D3DWrappers/CommandListWrapper.h>
#include <D3DWrappers/FenceWrapper.h>
#include <D3DWrappers/SwapChainWrapper.h>
#include <vector>
#include <dwmapi.h>
#include <thread>

#include <windows.h>
#define INITGUID // Ensure that EventTraceGuid is defined.
#include <evntrace.h>
#undef INITGUID
#include <tdh.h>
#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <sqlite3/sqlite3.h>
#include <filesystem>
#include <utils/TaskHandler.h>

// Link with Tdh.lib and Advapi32.lib
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

template<typename T>
T coalesce(T first) {
    return first;
}

template<typename T, typename... Args>
T coalesce(T first, Args... args) {
    return first != 0 ? first : coalesce(args...);
}

// Structure to uniquely identify an event
struct EventIdentifier {
    EventIdentifier() : m_providerId{}, m_id(0), m_version(0), padding(0) {

    }

    EventIdentifier(GUID providerId, USHORT id, UCHAR version) : padding(0) {
        this->m_providerId = providerId;
        this->m_id = id;
        this->m_version = version;
    }


    GUID m_providerId;
    USHORT m_id;
    UCHAR m_version;
    UCHAR padding; //Must explicitly pad otherwise memcmp won't work.
};

// Equality operator for EventIdentifier
bool operator==(const EventIdentifier& lhs, const EventIdentifier& rhs) {
    bool result = memcmp(reinterpret_cast<const void*>(&lhs), reinterpret_cast<const void*>(&rhs), sizeof(lhs)) == 0;
    return result;
}

// Hash function for EventIdentifier
static size_t HashEventIdentifier(const EventIdentifier& id) {
    size_t h1 = std::hash<unsigned long>{}(id.m_providerId.Data1);
    size_t h2 = std::hash<unsigned short>{}(id.m_providerId.Data2);
    size_t h3 = std::hash<unsigned short>{}(id.m_providerId.Data3);

    size_t h4 = 0;
    for (int i = 0; i < sizeof(id.m_providerId.Data4); ++i) {
        h4 = (h4 << 8) | id.m_providerId.Data4[i];
    }

    size_t result = h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (std::hash<USHORT>{}(id.m_id) << 4) ^ (std::hash<UCHAR>{}(id.m_version) << 5);
    return result;
}

// Specialize std::hash for EventIdentifier
namespace std {
    template <>
    struct hash<EventIdentifier> {
        std::size_t operator()(const EventIdentifier& id) const {
            return HashEventIdentifier(id);
        }
    };
}

// Specialize std::equals for EventIdentifier
namespace {
    struct EventIdentifierEqual {
        __forceinline bool operator()(const EventIdentifier& lhs, const EventIdentifier& rhs) const {
            bool result = memcmp(reinterpret_cast<const void*>(&lhs), reinterpret_cast<const void*>(&rhs), sizeof(lhs)) == 0;
            return result;
        }
    };
}

// Structure to hold event metadata
struct EventMetadata {
    //Below need to remain contiguous
    GUID m_providerId;
    USHORT m_eventId;
    UCHAR m_version;
    UCHAR padding;
    //Above need to remain contiguous
    std::string m_decodingSource;
    std::wstring m_providerName;
    std::wstring m_levelName;
    std::wstring m_channelName;
    std::wstring m_keywordsName;
    std::wstring m_providerMessage;
    std::wstring m_eventMessage;
    GUID m_providerGuid;
    std::wstring m_taskName;
    std::wstring m_opCodeName;
    std::vector<std::pair<std::wstring, std::string>> m_properties;
};

struct EventData {
    //Below need to remain contiguous
    GUID m_providerId;
    USHORT m_eventId;
    UCHAR m_version;
    UCHAR padding;
    //Above need to remain contiguous
    uint64_t timestamp;
    std::vector<std::pair<std::wstring, std::wstring>> m_properties;
};

bool operator==(const EventMetadata& lhs, const EventMetadata& rhs) {
    return memcmp(reinterpret_cast<const void*>(&lhs.m_providerId), reinterpret_cast<const void*>(&rhs.m_providerId), sizeof(lhs.m_providerId) + sizeof(lhs.m_eventId) + sizeof(lhs.m_version)) == 0;
}

// Global map to store event metadata
std::unordered_map<EventIdentifier, EventMetadata, std::hash<EventIdentifier>, ::EventIdentifierEqual> m_eventMetadataMap;

// Helper function to convert std::wstring to std::string
void ConvertWStringToString(const std::wstring& wstr, std::string* pStr) {
    int byteCount = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, nullptr, nullptr);
    byte* pBuffer = new byte[byteCount];
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, reinterpret_cast<char*>(pBuffer), byteCount, nullptr, nullptr);
    *pStr = reinterpret_cast<char*>(pBuffer);
    delete[] pBuffer;
}

// Helper function to convert std::string to std::wstring
void ConvertStringToWString(const std::string& str, std::wstring* pWstr) {
    int wcharsNum = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    wchar_t* pBuffer = new wchar_t[wcharsNum];
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, pBuffer, wcharsNum);
    *pWstr = pBuffer;
    delete[] pBuffer;
}

// Function to get the property data type as a string
std::string GetPropertyDataType(EVENT_PROPERTY_INFO& propInfo) {
    switch (propInfo.nonStructType.InType) {
    case TDH_INTYPE_NULL:
        return "NULL";
    case TDH_INTYPE_UNICODESTRING:
        return "UNICODESTRING";
    case TDH_INTYPE_ANSISTRING:
        return "ANSISTRING";
    case TDH_INTYPE_INT8:
        return "INT8";
    case TDH_INTYPE_UINT8:
        return "UINT8";
    case TDH_INTYPE_INT16:
        return "INT16";
    case TDH_INTYPE_UINT16:
        return "UINT16";
    case TDH_INTYPE_INT32:
        return "INT32";
    case TDH_INTYPE_UINT32:
        return "UINT32";
    case TDH_INTYPE_INT64:
        return "INT64";
    case TDH_INTYPE_UINT64:
        return "UINT64";
    case TDH_INTYPE_FLOAT:
        return "FLOAT";
    case TDH_INTYPE_DOUBLE:
        return "DOUBLE";
    case TDH_INTYPE_BOOLEAN:
        return "BOOLEAN";
    case TDH_INTYPE_BINARY:
        return "BINARY";
    case TDH_INTYPE_GUID:
        return "GUID";
    case TDH_INTYPE_POINTER:
        return "POINTER";
    case TDH_INTYPE_FILETIME:
        return "FILETIME";
    case TDH_INTYPE_SYSTEMTIME:
        return "SYSTEMTIME";
    case TDH_INTYPE_SID:
        return "SID";
    case TDH_INTYPE_HEXINT32:
        return "HEXINT32";
    case TDH_INTYPE_HEXINT64:
        return "HEXINT64";
    case TDH_INTYPE_MANIFEST_COUNTEDSTRING:
        return "MANIFEST_COUNTEDSTRING";
    case TDH_INTYPE_MANIFEST_COUNTEDANSISTRING:
        return "MANIFEST_COUNTEDANSISTRING";
    case TDH_INTYPE_RESERVED24:
        return "RESERVED24";
    case TDH_INTYPE_MANIFEST_COUNTEDBINARY:
        return "MANIFEST_COUNTEDBINARY";
    case TDH_INTYPE_COUNTEDSTRING:
        return "COUNTEDSTRING";
    case TDH_INTYPE_COUNTEDANSISTRING:
        return "COUNTEDANSISTRING";
    case TDH_INTYPE_REVERSEDCOUNTEDSTRING:
        return "REVERSEDCOUNTEDSTRING";
    case TDH_INTYPE_REVERSEDCOUNTEDANSISTRING:
        return "REVERSEDCOUNTEDANSISTRING";
    case TDH_INTYPE_NONNULLTERMINATEDSTRING:
        return "NONNULLTERMINATEDSTRING";
    case TDH_INTYPE_NONNULLTERMINATEDANSISTRING:
        return "NONNULLTERMINATEDANSISTRING";
    case TDH_INTYPE_UNICODECHAR:
        return "UNICODECHAR";
    case TDH_INTYPE_ANSICHAR:
        return "ANSICHAR";
    case TDH_INTYPE_SIZET:
        return "SIZET";
    case TDH_INTYPE_HEXDUMP:
        return "HEXDUMP";
    case TDH_INTYPE_WBEMSID:
        return "WBEMSID";
    default:
        return "UNKNOWN";
    }
}

// Function to collect event metadata
void CollectEventMetadata(PEVENT_RECORD pEventRecord) {
    EventIdentifier id{ pEventRecord->EventHeader.ProviderId, pEventRecord->EventHeader.EventDescriptor.Id, pEventRecord->EventHeader.EventDescriptor.Version };
    if (m_eventMetadataMap.contains(id)) {
        return; // Already handled
    }

    TRACE_EVENT_INFO* pEventInfo = nullptr;
    ULONG bufferSize = 0;
    ULONG status = TdhGetEventInformation(pEventRecord, 0, nullptr, pEventInfo, &bufferSize);
    if (status == ERROR_INSUFFICIENT_BUFFER) {
        pEventInfo = (TRACE_EVENT_INFO*)malloc(bufferSize);
        if (pEventInfo == nullptr) {
            return; // Failed to allocate memory for event info
        }

        status = TdhGetEventInformation(pEventRecord, 0, nullptr, pEventInfo, &bufferSize);
    }

    if (status != ERROR_SUCCESS) {
        free(pEventInfo);
        return; // TdhGetEventInformation failed
    }

    EventMetadata eventMeta;
    eventMeta.m_providerId = pEventRecord->EventHeader.ProviderId;
    eventMeta.m_providerGuid = pEventInfo->ProviderGuid;
    eventMeta.m_eventId = pEventInfo->EventDescriptor.Id;
    eventMeta.m_version = pEventInfo->EventDescriptor.Version;
    if (pEventInfo->ProviderNameOffset)
        eventMeta.m_providerName = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->ProviderNameOffset);
    if (pEventInfo->LevelNameOffset)
        eventMeta.m_levelName = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->LevelNameOffset);
    if (pEventInfo->ChannelNameOffset)
        eventMeta.m_channelName = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->ChannelNameOffset);
    if (pEventInfo->KeywordsNameOffset)
        eventMeta.m_keywordsName = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->KeywordsNameOffset);
    if (pEventInfo->DecodingSource != DecodingSourceWPP) {
        if (pEventInfo->TaskNameOffset)
            eventMeta.m_taskName = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->TaskNameOffset);
        if (pEventInfo->OpcodeNameOffset)
            eventMeta.m_opCodeName = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->OpcodeNameOffset);
    }
    if (pEventInfo->EventMessageOffset)
        eventMeta.m_eventMessage = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->EventMessageOffset);
    if (pEventInfo->ProviderMessageOffset)
        eventMeta.m_providerMessage = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->ProviderMessageOffset);

    for (ULONG i = 0; i < pEventInfo->TopLevelPropertyCount; i++) {
        PROPERTY_DATA_DESCRIPTOR propertyDescriptor;
        ZeroMemory(&propertyDescriptor, sizeof(PROPERTY_DATA_DESCRIPTOR));
        propertyDescriptor.PropertyName = (ULONGLONG)((PBYTE)pEventInfo + pEventInfo->EventPropertyInfoArray[i].NameOffset);

        ULONG propertyBufferSize = 0;
        status = TdhGetPropertySize(pEventRecord, 0, nullptr, 1, &propertyDescriptor, &propertyBufferSize);
        if (status == ERROR_SUCCESS) {
            std::vector<BYTE> propertyBuffer(propertyBufferSize);
            status = TdhGetProperty(pEventRecord, 0, nullptr, 1, &propertyDescriptor, propertyBufferSize, propertyBuffer.data());
            if (status == ERROR_SUCCESS) {
                std::wstring propertyName = (PWCHAR)((PBYTE)pEventInfo + pEventInfo->EventPropertyInfoArray[i].NameOffset);
                eventMeta.m_properties.push_back({ propertyName, GetPropertyDataType(pEventInfo->EventPropertyInfoArray[i]) });
            }
        }
    }

    m_eventMetadataMap[id] = eventMeta;
    free(pEventInfo);
}

// Callback function for processing events
void WINAPI EventRecordCallback(PEVENT_RECORD pEventRecord) {
    CollectEventMetadata(pEventRecord);
}

// Function to convert GUID to string
std::string GuidToString(const GUID& guid) {
    char buffer[64] = { 0 };
    snprintf(buffer, sizeof(buffer),
        "%08x-%04x-%04x-%04x-%012llx",
        guid.Data1, guid.Data2, guid.Data3,
        (guid.Data4[0] << 8) | guid.Data4[1],
        *(unsigned long long*)(guid.Data4 + 2));
    return std::string(buffer);
}

// Callback function for processing events
void WINAPI BackgroundEventRecordCallback(PEVENT_RECORD pEventRecord) {
    static_cast<std::function<void(PEVENT_RECORD pEventRecord)> *>(pEventRecord->UserContext)->operator()(pEventRecord);
}

/// <summary>
/// Modified example code from: https://learn.microsoft.com/en-us/windows/win32/etw/using-tdhformatproperty-to-consume-event-data
/// </summary>
class DecoderContext
{
public:

    TRACEHANDLE m_traceHandle;
    /*
    Initialize the decoder context.
    Sets up the TDH_CONTEXT array that will be used for decoding.
    */
    explicit DecoderContext(std::deque<EventData> &events, EventIdentifier &idFilter, size_t requestedCount,
        _In_opt_ LPCWSTR szTmfSearchPath) : m_events(events), m_idFilter(idFilter), m_requestedCount(requestedCount)
    {
        TDH_CONTEXT* p = m_tdhContext;

        if (szTmfSearchPath != nullptr)
        {
            p->ParameterValue = reinterpret_cast<UINT_PTR>(szTmfSearchPath);
            p->ParameterType = TDH_CONTEXT_WPP_TMFSEARCHPATH;
            p->ParameterSize = 0;
            p += 1;
        }

        m_tdhContextCount = static_cast<BYTE>(p - m_tdhContext);
    }

    /*
    Decode and print the data for an event.
    Might throw an exception for out-of-memory conditions. Caller should catch
    the exception before returning from the ProcessTrace callback.
    */
    void PrintEventRecord(
        _In_ EVENT_RECORD* pEventRecord)
    {
        if (m_events.size() == m_requestedCount) {
            m_requestedCount = 0;
            CloseTrace(m_traceHandle);
            return;
        }
        else if (m_requestedCount == 0) {
            return;
        }
        if (pEventRecord->EventHeader.EventDescriptor.Opcode == EVENT_TRACE_TYPE_INFO &&
            pEventRecord->EventHeader.ProviderId == EventTraceGuid)
        {
            /*
            The first event in every ETL file contains the data from the file header.
            This is the same data as was returned in the EVENT_TRACE_LOGFILEW by
            OpenTrace. Since we've already seen this information, we'll skip this
            event.
            */
            return;
        }
        EventIdentifier id{pEventRecord->EventHeader.ProviderId, pEventRecord->EventHeader.EventDescriptor.Id, pEventRecord->EventHeader.EventDescriptor.Version};
        if (id != m_idFilter)
            return;
        m_events.emplace_back(EventData{id.m_providerId, id.m_id, id.m_version, 0, static_cast<uint64_t>(pEventRecord->EventHeader.TimeStamp.QuadPart) });
        // Reset state to process a new event.
        m_pEvent = pEventRecord;
        m_pbData = static_cast<BYTE const*>(m_pEvent->UserData);
        m_pbDataEnd = m_pbData + m_pEvent->UserDataLength;
        m_pointerSize =
            m_pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER
            ? 4
            : m_pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER
            ? 8
            : sizeof(void*); // Ambiguous, assume size of the decoder's pointer.

        // There is a lot of information available in the event even without decoding,
        // including timestamp, PID, TID, provider ID, activity ID, and the raw data.
        // including timestamp, PID, TID, provider ID, activity ID, and the raw data.
        
        if (IsWppEvent())
        {
            PrintWppEvent();
        }
        else
        {
            PrintNonWppEvent();
        }
    }

private:

    /*
    Print the primary properties for a WPP event.
    */
    void PrintWppEvent()
    {
        return; //ETL Lens: Not handled for now.
        ///*
        //TDH supports a set of known properties for WPP events:
        //- "Version": UINT32 (usually 0)
        //- "TraceGuid": GUID
        //- "GuidName": UNICODESTRING (module name)
        //- "GuidTypeName": UNICODESTRING (source file name and line number)
        //- "ThreadId": UINT32
        //- "SystemTime": SYSTEMTIME
        //- "UserTime": UINT32
        //- "KernelTime": UINT32
        //- "SequenceNum": UINT32
        //- "ProcessId": UINT32
        //- "CpuNumber": UINT32
        //- "Indent": UINT32
        //- "FlagsName": UNICODESTRING
        //- "LevelName": UNICODESTRING
        //- "FunctionName": UNICODESTRING
        //- "ComponentName": UNICODESTRING
        //- "SubComponentName": UNICODESTRING
        //- "FormattedString": UNICODESTRING
        //- "RawSystemTime": FILETIME
        //- "ProviderGuid": GUID (usually 0)
        //*/

        //// Use TdhGetProperty to get the properties we need.
        //wprintf(L" ");
        //PrintWppStringProperty(L"GuidName"); // Module name (WPP's "CurrentDir" variable)
        //wprintf(L" ");
        //PrintWppStringProperty(L"GuidTypeName"); // Source code file name + line number
        //wprintf(L" ");
        //PrintWppStringProperty(L"FunctionName");
        //wprintf(L"\n");
        //PrintIndent();
        //PrintWppStringProperty(L"FormattedString");
        //wprintf(L"\n");
    }

    /*
    Print the value of the given UNICODESTRING property.
    */
    void PrintWppStringProperty(_In_z_ LPCWSTR szPropertyName)
    {
        PROPERTY_DATA_DESCRIPTOR pdd = { reinterpret_cast<UINT_PTR>(szPropertyName) };

        ULONG status;
        ULONG cb = 0;
        status = TdhGetPropertySize(
            m_pEvent,
            m_tdhContextCount,
            m_tdhContextCount ? m_tdhContext : nullptr,
            1,
            &pdd,
            &cb);
        if (status == ERROR_SUCCESS)
        {
            if (m_propertyBuffer.size() < cb / 2)
            {
                m_propertyBuffer.resize(cb / 2);
            }

            status = TdhGetProperty(
                m_pEvent,
                m_tdhContextCount,
                m_tdhContextCount ? m_tdhContext : nullptr,
                1,
                &pdd,
                cb,
                reinterpret_cast<BYTE*>(m_propertyBuffer.data()));
        }

        if (status != ERROR_SUCCESS)
        {
            wprintf(L"[TdhGetProperty(%ls) error %u]", szPropertyName, status);
        }
        else
        {
            // Print the FormattedString property data (nul-terminated
            // wchar_t string).
            wprintf(L"%ls", m_propertyBuffer.data());
        }
    }

    /*
    Use TdhGetEventInformation to obtain information about this event
    (including the names and types of the event's properties). Print some
    basic information about the event (provider name, event name), then print
    each property (using TdhFormatProperty to format each property value).
    */
    void PrintNonWppEvent()
    {
        ULONG status;
        ULONG cb;

        // Try to get event decoding information from TDH.
        cb = static_cast<ULONG>(m_teiBuffer.size());
        status = TdhGetEventInformation(
            m_pEvent,
            m_tdhContextCount,
            m_tdhContextCount ? m_tdhContext : nullptr,
            reinterpret_cast<TRACE_EVENT_INFO*>(m_teiBuffer.data()),
            &cb);
        if (status == ERROR_INSUFFICIENT_BUFFER)
        {
            m_teiBuffer.resize(cb);
            status = TdhGetEventInformation(
                m_pEvent,
                m_tdhContextCount,
                m_tdhContextCount ? m_tdhContext : nullptr,
                reinterpret_cast<TRACE_EVENT_INFO*>(m_teiBuffer.data()),
                &cb);
        }

        if (status != ERROR_SUCCESS)
        {
            // TdhGetEventInformation failed so there isn't a lot we can do.
            // The provider ID might be helpful in tracking down the right
            // manifest or TMF path.
        }
        else
        {
            // TDH found decoding information. Print some basic info about the event,
            // then format the event contents.

            TRACE_EVENT_INFO const* const pTei =
                reinterpret_cast<TRACE_EVENT_INFO const*>(m_teiBuffer.data());
            
            if (IsStringEvent())
            {
                // The event was written using EventWriteString.
                // We'll handle it later.
            }
            else
            {
                // The event is a MOF, manifest, or TraceLogging event.

                // To help resolve PropertyParamCount and PropertyParamLength,
                // we will record the values of all integer properties as we
                // reach them. Before we start, clear out any old values and
                // resize the vector with room for the new values.
                m_integerValues.clear();
                m_integerValues.resize(pTei->PropertyCount);

                // Recursively print the event's properties.
                PrintProperties(0, pTei->TopLevelPropertyCount);
            }
        }

        if (IsStringEvent())
        {
            // The event was written using EventWriteString.
            // We can print it whether or not we have decoding information.
            LPCWSTR pchData = static_cast<LPCWSTR>(m_pEvent->UserData);
            unsigned cchData = m_pEvent->UserDataLength / 2;
            
            // It's probably nul-terminated, but just in case, limit to cchData chars.
            m_events.back().m_properties.emplace_back(std::make_pair(L"WriteString", std::wstring(pchData, cchData)));
        }


    }

    /*
    Prints out the values of properties from begin..end.
    Called by PrintEventRecord for the top-level properties.
    If there are structures, this will be called recursively for the child
    properties.
    */
    void PrintProperties(unsigned propBegin, unsigned propEnd)
    {
        TRACE_EVENT_INFO const* const pTei =
            reinterpret_cast<TRACE_EVENT_INFO const*>(m_teiBuffer.data());

        for (unsigned propIndex = propBegin; propIndex != propEnd; propIndex += 1)
        {
            EVENT_PROPERTY_INFO const& epi = pTei->EventPropertyInfoArray[propIndex];

            // If this property is a scalar integer, remember the value in case it
            // is needed for a subsequent property's length or count.
            if (0 == (epi.Flags & (PropertyStruct | PropertyParamCount)) &&
                epi.count == 1)
            {
                switch (epi.nonStructType.InType)
                {
                case TDH_INTYPE_INT8:
                case TDH_INTYPE_UINT8:
                    if ((m_pbDataEnd - m_pbData) >= 1)
                    {
                        m_integerValues[propIndex] = *m_pbData;
                    }
                    break;
                case TDH_INTYPE_INT16:
                case TDH_INTYPE_UINT16:
                    if ((m_pbDataEnd - m_pbData) >= 2)
                    {
                        m_integerValues[propIndex] = *reinterpret_cast<UINT16 const UNALIGNED*>(m_pbData);
                    }
                    break;
                case TDH_INTYPE_INT32:
                case TDH_INTYPE_UINT32:
                case TDH_INTYPE_HEXINT32:
                    if ((m_pbDataEnd - m_pbData) >= 4)
                    {
                        auto val = *reinterpret_cast<UINT32 const UNALIGNED*>(m_pbData);
                        m_integerValues[propIndex] = static_cast<USHORT>(val > 0xffffu ? 0xffffu : val);
                    }
                    break;
                }
            }

            std::wstring propertyName = epi.NameOffset ? TeiString(epi.NameOffset) : L"(noname)";
            std::wstring propertyValue = L"";

            // We recorded the values of all previous integer properties just
            // in case we need to determine the property length or count.
            USHORT const propLength =
                epi.nonStructType.OutType == TDH_OUTTYPE_IPV6 &&
                epi.nonStructType.InType == TDH_INTYPE_BINARY &&
                epi.length == 0 &&
                (epi.Flags & (PropertyParamLength | PropertyParamFixedLength)) == 0
                ? 16 // special case for incorrectly-defined IPV6 addresses
                : (epi.Flags & PropertyParamLength)
                ? m_integerValues[epi.lengthPropertyIndex] // Look up the value of a previous property
                : epi.length;
            USHORT const arrayCount =
                (epi.Flags & PropertyParamCount)
                ? m_integerValues[epi.countPropertyIndex] // Look up the value of a previous property
                : epi.count;

            // Note that PropertyParamFixedCount is a new flag and is ignored
            // by many decoders. Without the PropertyParamFixedCount flag,
            // decoders will assume that a property is an array if it has
            // either a count parameter or a fixed count other than 1. The
            // PropertyParamFixedCount flag allows for fixed-count arrays with
            // one element to be propertly decoded as arrays.
            bool isArray =
                1 != arrayCount ||
                0 != (epi.Flags & (PropertyParamCount | PropertyParamFixedCount));
            if (isArray)
            {
                propertyValue = std::vformat(L"Array[{}]", std::make_wformat_args(arrayCount)); //ETL Lens: Need to actually implement this part.
            }

            PEVENT_MAP_INFO pMapInfo = nullptr;

            // Treat non-array properties as arrays with one element.
            for (unsigned arrayIndex = 0; arrayIndex != arrayCount; arrayIndex += 1)
            {
                if (isArray)
                {
                    break;//ETL Lens: Need to actually implement this part.
                }

                if (epi.Flags & PropertyStruct)
                {
                    propertyValue = L"Struct";//ETL Lens: Need to actually implement this part.
                    break;
                }

                // If the property has an associated map (i.e. an enumerated type),
                // try to look up the map data. (If this is an array, we only need
                // to do the lookup on the first iteration.)
                if (epi.nonStructType.MapNameOffset != 0 && arrayIndex == 0)
                {
                    switch (epi.nonStructType.InType)
                    {
                    case TDH_INTYPE_UINT8:
                    case TDH_INTYPE_UINT16:
                    case TDH_INTYPE_UINT32:
                    case TDH_INTYPE_HEXINT32:
                        if (m_mapBuffer.size() == 0)
                        {
                            m_mapBuffer.resize(sizeof(EVENT_MAP_INFO));
                        }

                        for (;;)
                        {
                            ULONG cbBuffer = static_cast<ULONG>(m_mapBuffer.size());
                            ULONG status = TdhGetEventMapInformation(
                                m_pEvent,
                                const_cast<LPWSTR>(TeiString(epi.nonStructType.MapNameOffset)),
                                reinterpret_cast<PEVENT_MAP_INFO>(m_mapBuffer.data()),
                                &cbBuffer);

                            if (status == ERROR_INSUFFICIENT_BUFFER &&
                                m_mapBuffer.size() < cbBuffer)
                            {
                                m_mapBuffer.resize(cbBuffer);
                                continue;
                            }
                            else if (status == ERROR_SUCCESS)
                            {
                                pMapInfo = reinterpret_cast<PEVENT_MAP_INFO>(m_mapBuffer.data());
                            }

                            break;
                        }
                        break;
                    }
                }

                bool useMap = pMapInfo != nullptr;

                // Loop because we may need to retry the call to TdhFormatProperty.
                for (;;)
                {
                    ULONG cbBuffer = static_cast<ULONG>(m_propertyBuffer.size() * 2);
                    USHORT cbUsed = 0;
                    ULONG status;

                    if (0 == propLength &&
                        epi.nonStructType.InType == TDH_INTYPE_NULL)
                    {
                        // TdhFormatProperty doesn't handle INTYPE_NULL.
                        if (m_propertyBuffer.empty())
                        {
                            m_propertyBuffer.push_back(0);
                        }
                        m_propertyBuffer[0] = 0;
                        status = ERROR_SUCCESS;
                    }
                    else if (
                        0 == propLength &&
                        0 != (epi.Flags & (PropertyParamLength | PropertyParamFixedLength)) &&
                        (epi.nonStructType.InType == TDH_INTYPE_UNICODESTRING ||
                            epi.nonStructType.InType == TDH_INTYPE_ANSISTRING))
                    {
                        // TdhFormatProperty doesn't handle zero-length counted strings.
                        if (m_propertyBuffer.empty())
                        {
                            m_propertyBuffer.push_back(0);
                        }
                        m_propertyBuffer[0] = 0;
                        status = ERROR_SUCCESS;
                    }
                    else
                    {
                        status = TdhFormatProperty(
                            const_cast<TRACE_EVENT_INFO*>(pTei),
                            useMap ? pMapInfo : nullptr,
                            m_pointerSize,
                            epi.nonStructType.InType,
                            static_cast<USHORT>(
                                epi.nonStructType.OutType == TDH_OUTTYPE_NOPRINT
                                ? TDH_OUTTYPE_NULL
                                : epi.nonStructType.OutType),
                            propLength,
                            static_cast<USHORT>(m_pbDataEnd - m_pbData),
                            const_cast<PBYTE>(m_pbData),
                            &cbBuffer,
                            m_propertyBuffer.data(),
                            &cbUsed);
                    }

                    if (status == ERROR_INSUFFICIENT_BUFFER &&
                        m_propertyBuffer.size() < cbBuffer / 2)
                    {
                        // Try again with a bigger buffer.
                        m_propertyBuffer.resize(cbBuffer / 2);
                        continue;
                    }
                    else if (status == ERROR_EVT_INVALID_EVENT_DATA && useMap)
                    {
                        // If the value isn't in the map, TdhFormatProperty treats it
                        // as an error instead of just putting the number in. We'll
                        // try again with no map.
                        useMap = false;
                        continue;
                    }
                    else if (status != ERROR_SUCCESS)
                    {
                        wprintf(L" [ERROR:TdhFormatProperty:%lu]\n", status);
                    }
                    else
                    {
                        propertyValue = m_propertyBuffer.data();
                        m_pbData += cbUsed;
                    }

                    break;
                }
            }
            m_events.back().m_properties.emplace_back(std::make_pair(propertyName, propertyValue));
        }
    }

    /*
    Returns true if the current event has the EVENT_HEADER_FLAG_STRING_ONLY
    flag set.
    */
    bool IsStringEvent() const
    {
        return (m_pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_STRING_ONLY) != 0;
    }

    /*
    Returns true if the current event has the EVENT_HEADER_FLAG_TRACE_MESSAGE
    flag set.
    */
    bool IsWppEvent() const
    {
        return (m_pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_TRACE_MESSAGE) != 0;
    }

    /*
    Converts a TRACE_EVENT_INFO offset (e.g. TaskNameOffset) into a string.
    */
    _Ret_z_ LPCWSTR TeiString(unsigned offset)
    {
        return reinterpret_cast<LPCWSTR>(m_teiBuffer.data() + offset);
    }

private:

    TDH_CONTEXT m_tdhContext[1]; // May contain TDH_CONTEXT_WPP_TMFSEARCHPATH.
    BYTE m_tdhContextCount;  // 1 if a TMF search path is present.
    BYTE m_pointerSize;
    EVENT_RECORD* m_pEvent;      // The event we're currently printing.
    BYTE const* m_pbData;        // Position of the next byte of event data to be consumed.
    BYTE const* m_pbDataEnd;     // Position of the end of the event data.
    std::vector<USHORT> m_integerValues; // Stored property values for resolving array lengths.
    std::vector<BYTE> m_teiBuffer; // Buffer for TRACE_EVENT_INFO data.
    std::vector<wchar_t> m_propertyBuffer; // Buffer for the string returned by TdhFormatProperty.
    std::vector<BYTE> m_mapBuffer; // Buffer for the data returned by TdhGetEventMapInformation.
    std::deque<EventData>& m_events;
    EventIdentifier& m_idFilter;
    size_t m_requestedCount;
};


// Callback function for processing events
void WINAPI EventRecordCallbackBackground(PEVENT_RECORD pEventRecord) {
    reinterpret_cast<DecoderContext*>(pEventRecord->UserContext)->PrintEventRecord(pEventRecord);
}

std::map<LONG, std::string> styleNames = {
    {WS_OVERLAPPED, "WS_OVERLAPPED"},
    {WS_POPUP, "WS_POPUP"},
    {WS_CHILD, "WS_CHILD"},
    {WS_MINIMIZE, "WS_MINIMIZE"},
    {WS_VISIBLE, "WS_VISIBLE"},
    {WS_DISABLED, "WS_DISABLED"},
    {WS_CLIPSIBLINGS, "WS_CLIPSIBLINGS"},
    {WS_CLIPCHILDREN, "WS_CLIPCHILDREN"},
    {WS_MAXIMIZE, "WS_MAXIMIZE"},
    {WS_CAPTION, "WS_CAPTION"},
    {WS_BORDER, "WS_BORDER"},
    {WS_DLGFRAME, "WS_DLGFRAME"},
    {WS_VSCROLL, "WS_VSCROLL"},
    {WS_HSCROLL, "WS_HSCROLL"},
    {WS_SYSMENU, "WS_SYSMENU"},
    {WS_THICKFRAME, "WS_THICKFRAME"},
    {WS_GROUP, "WS_GROUP"},
    {WS_TABSTOP, "WS_TABSTOP"},
    {WS_MINIMIZEBOX, "WS_MINIMIZEBOX"},
    {WS_MAXIMIZEBOX, "WS_MAXIMIZEBOX"},
    {WS_TILED, "WS_TILED"},
    {WS_ICONIC, "WS_ICONIC"},
    {WS_SIZEBOX, "WS_SIZEBOX"},
    {WS_TILEDWINDOW, "WS_TILEDWINDOW"},
    {WS_OVERLAPPEDWINDOW, "WS_OVERLAPPEDWINDOW"},
    {WS_POPUPWINDOW, "WS_POPUPWINDOW"},
    {WS_CHILDWINDOW, "WS_CHILDWINDOW"}
};

std::map<LONG, std::string> exStyleNames = {
    {WS_EX_DLGMODALFRAME, "WS_EX_DLGMODALFRAME"},
    {WS_EX_NOPARENTNOTIFY, "WS_EX_NOPARENTNOTIFY"},
    {WS_EX_TOPMOST, "WS_EX_TOPMOST"},
    {WS_EX_ACCEPTFILES, "WS_EX_ACCEPTFILES"},
    {WS_EX_TRANSPARENT, "WS_EX_TRANSPARENT"},
    {WS_EX_MDICHILD, "WS_EX_MDICHILD"},
    {WS_EX_TOOLWINDOW, "WS_EX_TOOLWINDOW"},
    {WS_EX_WINDOWEDGE, "WS_EX_WINDOWEDGE"},
    {WS_EX_CLIENTEDGE, "WS_EX_CLIENTEDGE"},
    {WS_EX_CONTEXTHELP, "WS_EX_CONTEXTHELP"},
    {WS_EX_RIGHT, "WS_EX_RIGHT"},
    {WS_EX_LEFT, "WS_EX_LEFT"},
    {WS_EX_RTLREADING, "WS_EX_RTLREADING"},
    {WS_EX_LTRREADING, "WS_EX_LTRREADING"},
    {WS_EX_LEFTSCROLLBAR, "WS_EX_LEFTSCROLLBAR"},
    {WS_EX_RIGHTSCROLLBAR, "WS_EX_RIGHTSCROLLBAR"},
    {WS_EX_CONTROLPARENT, "WS_EX_CONTROLPARENT"},
    {WS_EX_STATICEDGE, "WS_EX_STATICEDGE"},
    {WS_EX_APPWINDOW, "WS_EX_APPWINDOW"},
    {WS_EX_LAYERED, "WS_EX_LAYERED"},
    {WS_EX_NOINHERITLAYOUT, "WS_EX_NOINHERITLAYOUT"},
    {WS_EX_NOREDIRECTIONBITMAP, "WS_EX_NOREDIRECTIONBITMAP"},
    {WS_EX_LAYOUTRTL, "WS_EX_LAYOUTRTL"},
    {WS_EX_COMPOSITED, "WS_EX_COMPOSITED"},
    {WS_EX_NOACTIVATE, "WS_EX_NOACTIVATE"}
};

static int const NUM_FRAMES_IN_FLIGHT = 3;
struct FrameContext
{
    CommandAllocatorWrapper m_cmdAllocator;
    CommandListWrapper m_cmdList;
    FenceWrapper m_fence;

    FrameContext(QueueWrapper &queueWrapper) : m_cmdAllocator(queueWrapper, D3D12_COMMAND_LIST_TYPE_DIRECT), m_cmdList(m_cmdAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT), m_fence(queueWrapper) {

    }

    FrameContext(const FrameContext& other) = delete;
    FrameContext& operator=(const FrameContext& other) = delete;
    FrameContext(FrameContext&& other) = delete; //Members reference each other.
};


struct RenderContext {
    DeviceWrapper m_device;
    QueueWrapper m_queue;
    SwapChainWrapper m_swapchain;
    std::vector<FrameContext*> m_frameContexts;
    ComPtr<ID3D12DescriptorHeap> m_srvDescHeap;

    RenderContext(HWND hwnd, UINT width, UINT height) : m_device(), m_queue(m_device, D3D12_COMMAND_LIST_TYPE_DIRECT), m_swapchain(m_queue, hwnd, width, height, NUM_FRAMES_IN_FLIGHT), m_frameContexts{}, m_srvDescHeap(m_device.CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) {
        for (int i = 0; i < NUM_FRAMES_IN_FLIGHT; i++) {
            m_frameContexts.emplace_back(new FrameContext(m_queue));
        }
    }

    ~RenderContext() {
        for (auto* context : m_frameContexts)
            delete context;
    }
};

static bool g_SwapChainOccluded = false;
static std::atomic<bool> g_needsResize = false;
UINT g_resizeWidth, g_resizeHeight;
static RenderContext *g_pRenderContext = nullptr;
static SRWLOCK messageInfoLock = SRWLOCK_INIT;



void WaitForLastSubmittedFrame()
{
    FrameContext& frameCtx = *g_pRenderContext->m_frameContexts[g_pRenderContext->m_swapchain.GetCurrentFrameIndex() == 0
        ? NUM_FRAMES_IN_FLIGHT - 1 : g_pRenderContext->m_swapchain.GetCurrentFrameIndex() - 1];
    frameCtx.m_fence.WaitForFence();
}

FrameContext& WaitForNextFrameResources()
{
    FrameContext& frameCtx = *g_pRenderContext->m_frameContexts[g_pRenderContext->m_swapchain.GetCurrentFrameIndex()];
    frameCtx.m_fence.WaitForFence();

    return frameCtx;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int main(int, char**)
{
    std::string etlFilePath = "C:\\Users\\Pierre-Yves\\Documents\\etwtraces\\2024-07-01_23-34-42_Pierre-Yves.etl";
    std::wstring etlFilePathW;
    ConvertStringToWString(etlFilePath, &etlFilePathW);

    EVENT_TRACE_LOGFILE logFile = { 0 };    
    logFile.LoggerName = NULL;
    logFile.LogFileName = const_cast<LPWSTR>(etlFilePathW.c_str());
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logFile.EventRecordCallback = (PEVENT_RECORD_CALLBACK)(EventRecordCallback);

    TRACEHANDLE traceHandle = OpenTrace(&logFile);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
        std::cerr << "Failed to open trace" << std::endl;
        return 1;
    }

    ULONG status = ProcessTrace(&traceHandle, 1, NULL, NULL);
    if (status != ERROR_SUCCESS) {
        std::cerr << "ProcessTrace failed with status: " << status << std::endl;
        CloseTrace(traceHandle);
        return 1;
    }

    CloseTrace(traceHandle);

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ETL Lens", nullptr };
    ::RegisterClassExW(&wc);
    UINT width = 1280;
    UINT height = 800;
    HWND hwnd = ::CreateWindowExW(WS_EX_WINDOWEDGE, wc.lpszClassName, L"ETL Lens", WS_POPUP | WS_POPUPWINDOW | WS_THICKFRAME | WS_TILEDWINDOW | WS_CLIPSIBLINGS, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);
    DWORD allowDarkMode = 1;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &allowDarkMode, sizeof(allowDarkMode));
    /*  HWND other = FindWindow(NULL, _T("Microsoft Visual Studio"));
      LONG otherStyle = GetWindowLong(hwnd, GWL_STYLE);
      LONG otherExStyle = GetWindowLong(other, GWL_EXSTYLE);

      std::cout << "Styles: " << "\n";
      for (const auto& pair : styleNames) {
          if (otherStyle & pair.first) {
              std::cout << "\t" << pair.second << "\n";
          }
      }
      std::cout << "Extended Styles: " << "\n";
      for (const auto& pair : exStyleNames) {
          if (otherExStyle & pair.first) {
              std::cout << "\t" << pair.second << "\n";
          }
      }
      std::cout << std::endl;*/
    g_pRenderContext = new RenderContext(hwnd, width, height);

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.f;
        style.Colors[ImGuiCol_WindowBg].w = 1.f;
    }
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(g_pRenderContext->m_device.GetDevice(), NUM_FRAMES_IN_FLIGHT,
        DXGI_FORMAT_R8G8B8A8_UNORM, g_pRenderContext->m_srvDescHeap.Get(),
        g_pRenderContext->m_srvDescHeap.Get()->GetCPUDescriptorHandleForHeapStart(),
        g_pRenderContext->m_srvDescHeap.Get()->GetGPUDescriptorHandleForHeapStart());


    bool running = true;
    //std::thread renderThread([&running, &hwnd, &io] {
    TaskHandler<EventIdentifier, std::deque<EventData>> backgroundWorker([&running, &etlFilePathW](EventIdentifier&& filter, TaskHandler<EventIdentifier, std::deque<EventData>>* tH) -> bool {
        EventIdentifier filterId = filter;
        std::deque<EventData> events;
        DecoderContext context(events, filterId, 100, nullptr);
        EVENT_TRACE_LOGFILE logFile = { 0 };
        logFile.LoggerName = NULL;
        logFile.LogFileName = const_cast<LPWSTR>(etlFilePathW.c_str());
        logFile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        logFile.EventRecordCallback = (PEVENT_RECORD_CALLBACK)(EventRecordCallbackBackground);
        logFile.Context = &context;

        TRACEHANDLE traceHandle = OpenTrace(&logFile);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
            std::cerr << "Failed to open trace" << std::endl;
            return !running;
        }

        context.m_traceHandle = traceHandle;

        ULONG status = ProcessTrace(&traceHandle, 1, NULL, NULL);
        if (status != ERROR_SUCCESS) {
            std::cerr << "ProcessTrace failed with status: " << status << std::endl;
            CloseTrace(traceHandle);
            return !running;
        }

        CloseTrace(traceHandle);
        tH->PushOutput(std::move(events));
        return !running;
    });
    std::deque<EventData> uiEvents;
    std::vector<EventMetadata> items;
    items.reserve(m_eventMetadataMap.size());
    for (auto& pair : m_eventMetadataMap)
        items.push_back(pair.second);
    ImVec4 clear_color = ImVec4(0.f, 0.f, 0.f, 1.00f);
    EventMetadata noEvent{}; //Compare with all zero.
    EventMetadata selectedEvent{};
    while (running)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (LOWORD(msg.message) == WM_QUIT)
            {
                running = false;
                break;
            }
        }

        if (!running)
            break;

        if (g_needsResize && g_needsResize.exchange(false)) {
            AcquireSRWLockShared(&messageInfoLock);
            UINT width = g_resizeWidth;
            UINT height = g_resizeHeight;
            ReleaseSRWLockShared(&messageInfoLock);
            WaitForLastSubmittedFrame();
            g_pRenderContext->m_swapchain.Resize(g_resizeWidth, g_resizeHeight);
        }
        // Handle window screen locked
        if (g_SwapChainOccluded && g_pRenderContext->m_swapchain.Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        //UI Render
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowContentSize(ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
        if (ImGui::Begin("Main Window", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking)) {
            if (ImGui::BeginChild("Top Child", ImVec2(0, ImGui::GetWindowHeight() * 0.5f), ImGuiChildFlags_ResizeY)) {
                ImVec2 startPos = ImGui::GetCursorPos();
                if (ImGui::BeginTable("Events", 8, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Reorderable | ImGuiTableFlags_HighlightHoveredColumn | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Provider", ImGuiTableColumnFlags_PreferSortAscending);
                    ImGui::TableSetupColumn("Task", ImGuiTableColumnFlags_PreferSortAscending);
                    ImGui::TableSetupColumn("OpCode", ImGuiTableColumnFlags_PreferSortAscending);
                    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_PreferSortAscending);
                    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_PreferSortAscending);
                    ImGui::TableSetupColumn("Keywords", ImGuiTableColumnFlags_PreferSortAscending);
                    ImGui::TableSetupColumn("Event Id", ImGuiTableColumnFlags_PreferSortAscending);
                    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_PreferSortAscending);
                    ImGui::TableHeadersRow();

                    // Handle sorting
                    if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
                        if (sortSpecs->SpecsDirty) {
                            std::sort(items.begin(), items.end(), [&](const EventMetadata& a, const EventMetadata& b) -> bool {
                                for (int n = 0; n < sortSpecs->SpecsCount; n++) {
                                    const ImGuiTableColumnSortSpecs* spec = &sortSpecs->Specs[n];
                                    int delta = 0;
                                    switch (spec->ColumnIndex) {
                                    case 0: delta = a.m_providerName.compare(b.m_providerName); break;
                                    case 1: delta = a.m_taskName.compare(b.m_taskName); break;
                                    case 2: delta = a.m_opCodeName.compare(b.m_opCodeName); break;
                                    case 3: delta = a.m_levelName.compare(b.m_levelName); break;
                                    case 4: delta = a.m_channelName.compare(b.m_channelName); break;
                                    case 5: delta = a.m_keywordsName.compare(b.m_keywordsName); break;
                                    case 6: delta = (int)a.m_eventId - (int)b.m_eventId; break;
                                    case 7: delta = (int)a.m_version - (int)b.m_version; break;
                                    }
                                    if (delta > 0)
                                        return (spec->SortDirection == ImGuiSortDirection_Descending);
                                    if (delta < 0)
                                        return (spec->SortDirection == ImGuiSortDirection_Ascending);
                                }
                                return coalesce(a.m_providerName.compare(b.m_providerName), a.m_taskName.compare(b.m_taskName), (int)a.m_eventId - (int)b.m_eventId, (int)a.m_version - (int)b.m_version);
                                });
                            sortSpecs->SpecsDirty = false;
                        }
                    }

                    for (auto& metadata : items) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        std::string providerName;
                        ConvertWStringToString(metadata.m_providerName, &providerName);
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header)); 
                        std::string providerIdStr = GuidToString(metadata.m_providerId);
                        std::string label = std::vformat("{}###{}{}{}", std::make_format_args(providerName, providerIdStr, metadata.m_eventId, metadata.m_version));
                        if (ImGui::Selectable(label.c_str(), metadata == selectedEvent, ImGuiSelectableFlags_SpanAllColumns)) {
                            if (selectedEvent != metadata) {
                                selectedEvent = metadata;
                                uiEvents.clear();
                                backgroundWorker.PushInput(EventIdentifier{ selectedEvent.m_providerId, selectedEvent.m_eventId, selectedEvent.m_version });
                            }
                        }
                        ImGui::PopStyleColor();

                        ImGui::TableNextColumn();
                        std::string taskName;
                        ConvertWStringToString(metadata.m_taskName, &taskName);
                        ImGui::Text(taskName.c_str());

                        ImGui::TableNextColumn();
                        std::string opCode;
                        ConvertWStringToString(metadata.m_opCodeName, &opCode);
                        ImGui::Text(opCode.c_str());

                        ImGui::TableNextColumn();
                        std::string level;
                        ConvertWStringToString(metadata.m_levelName, &level);
                        ImGui::Text(level.c_str());

                        ImGui::TableNextColumn();
                        std::string channel;
                        ConvertWStringToString(metadata.m_channelName, &channel);
                        ImGui::Text(channel.c_str());

                        ImGui::TableNextColumn();
                        std::string keywords;
                        ConvertWStringToString(metadata.m_keywordsName, &keywords);
                        ImGui::Text(keywords.c_str());

                        ImGui::TableNextColumn();
                        ImGui::Text(std::to_string(metadata.m_eventId).c_str());

                        ImGui::TableNextColumn();
                        ImGui::Text(std::to_string(metadata.m_version).c_str());
                    }

                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();
            if (ImGui::BeginChild("Bottom", ImVec2(0,0), ImGuiChildFlags_Border)) {
                if (ImGui::BeginChild("Event Struct", ImVec2(ImGui::GetWindowWidth() * 0.1f, -1), ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX)) {
                    if (ImGui::BeginTable("Event Details", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Property");
                        ImGui::TableSetupColumn("Type");
                        ImGui::TableHeadersRow();
                        for (auto& pair : selectedEvent.m_properties) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            std::string name;
                            ConvertWStringToString(pair.first, &name);
                            ImGui::Text(name.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text(pair.second.c_str());
                        }

                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
                ImGui::SameLine();
                if (ImGui::BeginChild("Events", ImVec2(-1, -1), ImGuiChildFlags_Border | ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_AutoResizeX)) {
                    backgroundWorker.PopOutput(&uiEvents, false); //Update if thread has provided new ones.
                    if (selectedEvent != noEvent) {
                        if (ImGui::BeginTable("Events Instances", selectedEvent.m_properties.size() + 1,ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Reorderable | ImGuiTableFlags_SizingFixedFit, ImGui::GetWindowSize())) {
                            ImGui::TableSetupScrollFreeze(0, 1);
                            ImGui::TableSetupColumn("Timestamp");
                            for (const auto& pair : selectedEvent.m_properties) {
                                std::string name;
                                ConvertWStringToString(pair.first, &name);
                                ImGui::TableSetupColumn(name.c_str());
                            }
                            ImGui::TableHeadersRow();
                            for (auto& uiEvent : uiEvents) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                uint64_t selectableId = reinterpret_cast<uint64_t>(&uiEvent);
                                std::string text = std::vformat("{}###{}", std::make_format_args(uiEvent.timestamp, selectableId));
                                ImGui::Selectable(text.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                                std::string name;
                                int i = 0;
                                for (auto& pair : uiEvent.m_properties) {
                                    if (++i > selectedEvent.m_properties.size())
                                        break;
                                    ImGui::TableNextColumn();
                                    ConvertWStringToString(pair.second, &name);
                                    ImGui::Text(name.c_str());
                                }
                                if (selectedEvent.m_properties.size() > uiEvent.m_properties.size()) {
                                    for (int i = 0; i < selectedEvent.m_properties.size() - uiEvent.m_properties.size(); i++) {
                                        ImGui::TableNextColumn();
                                    }
                                }
                            }


                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopStyleVar(4);

        // Rendering
        ImGui::Render();

        FrameContext& frameCtx = WaitForNextFrameResources();
        frameCtx.m_cmdAllocator.GetCommandAllocator()->Reset();

        frameCtx.m_cmdList.Reset();
        frameCtx.m_cmdList.TransitionBarrier(g_pRenderContext->m_swapchain.GetCurrentBackBuffer().Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // Render Dear ImGui graphics
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        frameCtx.m_cmdList.ClearRenderTargetView(g_pRenderContext->m_swapchain.GetCurrentRtvDescriptorHandle(), clear_color_with_alpha);

        D3D12_CPU_DESCRIPTOR_HANDLE currentRtvDescriptorHandle = g_pRenderContext->m_swapchain.GetCurrentRtvDescriptorHandle();
        frameCtx.m_cmdList.OMSetRenderTargets(1, &currentRtvDescriptorHandle, FALSE, nullptr);

        ID3D12DescriptorHeap* const srvHeap = g_pRenderContext->m_srvDescHeap.Get();
        frameCtx.m_cmdList.GetCommandList()->SetDescriptorHeaps(1, &srvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), frameCtx.m_cmdList.GetCommandList().Get());
        frameCtx.m_cmdList.TransitionBarrier(g_pRenderContext->m_swapchain.GetCurrentBackBuffer().Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        frameCtx.m_cmdList.Close();

        ID3D12CommandList* const cmdList = frameCtx.m_cmdList.GetCommandList().Get();

        g_pRenderContext->m_queue.GetQueue()->ExecuteCommandLists(1, &cmdList);

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault(nullptr, (void*)frameCtx.m_cmdList.GetCommandList().Get());
        }

        // Present
        HRESULT hr = g_pRenderContext->m_swapchain.Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

        frameCtx.m_fence.Signal();
    }
    backgroundWorker.PushInput({GUID{}, 0, 0 });
    backgroundWorker.Join();
    //});

        //MSG msg;
        //while (::GetMessage(&msg, nullptr, 0U, 0U))
        //{
        //    ::TranslateMessage(&msg);
        //    ::DispatchMessage(&msg);
        //}
        //running = false;
        //renderThread.join(); //We've quit, time to join so we can cleanup.

    WaitForLastSubmittedFrame();

    // Cleanup
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();



    delete g_pRenderContext;
    g_pRenderContext = nullptr;
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

#ifdef DX12_ENABLE_DEBUG_LAYER
    IDXGIDebug1* pDebug = nullptr;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug))) {
        pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
        pDebug->Release();
    }
#endif

    return 0;
}


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZING:
    {
        RECT* rect = reinterpret_cast<RECT*>(lParam);
        g_resizeWidth = (UINT)(rect->right - rect->left);
        g_resizeHeight = (UINT)(rect->bottom - rect->top);
        g_needsResize = true;
        return 0;
    }
    case WM_SIZE:
        if (g_pRenderContext != nullptr && g_pRenderContext->m_device.GetDevice() != nullptr && wParam != SIZE_MINIMIZED)
        {
            AcquireSRWLockExclusive(&messageInfoLock);
            g_resizeWidth = (UINT)LOWORD(lParam);
            g_resizeHeight = (UINT)HIWORD(lParam);
            ReleaseSRWLockExclusive(&messageInfoLock);
            g_needsResize.store(true);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}