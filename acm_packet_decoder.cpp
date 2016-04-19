#include <memory>
#include <vector>
#include <locale>
#include <codecvt>
#include <windows.h>
#include <mmreg.h>
#include <msacm.h>
#include "../SDK/foobar2000.h"
#include "Helpers.h"

namespace {
    void acm_stream_close(HACMSTREAM h) { acmStreamClose(h, 0); }
}

class acm_packet_decoder: public packet_decoder {
    WORD                          m_wFormatTag;
    std::string                   m_codec_name;
    WAVEFORMATEX                  m_ofmt;
    DWORD                         m_channel_mask;        
    std::shared_ptr<HACMSTREAM__> m_stream;
    std::vector<BYTE>             m_sample_buffer;
public:
    acm_packet_decoder()
        : m_wFormatTag(0), m_channel_mask(0)
    {
        memset(&m_ofmt, 0, sizeof m_ofmt);
    }
    bool is_lpcm()
    {
        return m_wFormatTag == 1 || m_wFormatTag == 3;
    }
    static bool g_is_our_setup(const GUID &p_owner,  t_size p_param1,
                               const void *p_param2, t_size p_param2size)
    {
        auto setup = static_cast<const matroska_setup *>(p_param2);
        return p_owner == owner_matroska
            && p_param2size == sizeof(matroska_setup)
            && std::strncmp(setup->codec_id, "A_MS/ACM", 8) == 0;
    }
    bool open_lpcm(const WAVEFORMATEX *wfx)
    {
        if (is_lpcm())
            return true;
        else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto wfxe = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wfx);
            m_channel_mask = wfxe->dwChannelMask;
            if (wfxe->SubFormat == KSDATAFORMAT_SUBTYPE_PCM
             || wfxe->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                m_wFormatTag = wfxe->SubFormat.Data1;
                return true;
            }
        }
        return false;
    }
    void open_acm(const WAVEFORMATEX *wfx)
    {
        m_ofmt.wFormatTag = WAVE_FORMAT_PCM;
        if (acmFormatSuggest(0, const_cast<WAVEFORMATEX *>(wfx),
                             &m_ofmt, sizeof m_ofmt,
                             ACM_FORMATSUGGESTF_NCHANNELS |
                             ACM_FORMATSUGGESTF_NSAMPLESPERSEC |
                             ACM_FORMATSUGGESTF_WFORMATTAG))
            throw std::runtime_error("acmFormatSuggest() failed");

        ACMFORMATTAGDETAILSW details = { sizeof(ACMFORMATTAGDETAILSW) };
        details.dwFormatTag = wfx->wFormatTag;
        if (acmFormatTagDetailsW(0, &details, ACM_FORMATTAGDETAILSF_FORMATTAG))
            throw std::runtime_error("acmFormatTagDetails() failed");
        std::wstring_convert<std::codecvt_utf8<wchar_t> > conv;
        m_codec_name = conv.to_bytes(details.szFormatTag);

        HACMSTREAM has;
        if (acmStreamOpen(&has, 0, const_cast<WAVEFORMATEX *>(wfx),
                          &m_ofmt, 0, 0, 0, 0))
            throw std::runtime_error("acmStreamOpen() failed");
        m_stream = std::shared_ptr<HACMSTREAM__>(has, acm_stream_close);
    }
    void open(const WAVEFORMATEX *wfx)
    {
        m_wFormatTag = wfx->wFormatTag;
        m_ofmt = *wfx;
        if (!open_lpcm(wfx)) open_acm(wfx);
    }
    void open(const GUID &p_owner, bool p_decode, t_size p_param1,
              const void *p_param2, t_size p_param2size,
              abort_callback &p_abort)
    {
        auto setup = static_cast<const matroska_setup *>(p_param2);
        open(static_cast<const WAVEFORMATEX *>(setup->codec_private));
    }
    void get_info_lpcm(file_info &p_info)
    {
        if (m_wFormatTag == 3)
            p_info.info_set("codec", "PCM (floating point)");
        else
            p_info.info_set("codec", "PCM");
        p_info.info_set("encoding", "lossless");
        p_info.info_set_int("bitspersample", m_ofmt.wBitsPerSample);
    }
    void get_info_acm(file_info &p_info)
    {
        p_info.info_set("codec",    m_codec_name.c_str());
        p_info.info_set("encoding", "lossy");
    }
    void get_info(file_info &p_info)
    {
        if (is_lpcm())
            get_info_lpcm(p_info);
        else
            get_info_acm(p_info);

        p_info.info_set_int("samplerate", m_ofmt.nSamplesPerSec);
        if (!m_channel_mask)
            p_info.info_set_int("channels",   m_ofmt.nChannels);
        else {
            auto s = Helpers::describe_channels(m_channel_mask);
            p_info.info_set("channels", s.c_str());
        }
    }
    void decode_lpcm(const void *p_buffer, t_size p_bytes,
                             audio_chunk &p_chunk, abort_callback &p_abort)
    {
        unsigned chanmask = m_channel_mask;
        if (!chanmask)
            chanmask = audio_chunk::g_guess_channel_config(m_ofmt.nChannels);
        if (m_wFormatTag == 1)
            p_chunk.set_data_fixedpoint(p_buffer,
                                        p_bytes,
                                        m_ofmt.nSamplesPerSec,
                                        m_ofmt.nChannels,
                                        m_ofmt.wBitsPerSample,
                                        chanmask);
        else
            p_chunk.set_data_floatingpoint_ex(p_buffer,
                                              p_bytes,
                                              m_ofmt.nSamplesPerSec,
                                              m_ofmt.nChannels,
                                              m_ofmt.wBitsPerSample,
                                              audio_chunk::FLAG_SIGNED,
                                              chanmask);
    }
    void decode_acm(const void *p_buffer, t_size p_bytes,
                            audio_chunk &p_chunk, abort_callback &p_abort)
    {
        DWORD num_obytes;
        if (acmStreamSize(m_stream.get(), p_bytes, &num_obytes,
                          ACM_STREAMSIZEF_SOURCE))
            throw std::runtime_error("acmStreamSize() failed");
        if (m_sample_buffer.size() < num_obytes)
            m_sample_buffer.resize(num_obytes);

        ACMSTREAMHEADER header = { sizeof(ACMSTREAMHEADER) };
        header.pbSrc = const_cast<LPBYTE>(static_cast<LPCBYTE>(p_buffer));
        header.cbSrcLength     = p_bytes;
        header.pbDst           = m_sample_buffer.data();
        header.cbDstLength     = m_sample_buffer.size();
        if (acmStreamPrepareHeader(m_stream.get(), &header, 0))
            throw std::runtime_error("acmStreamPrepareHeader() failed");
        if (acmStreamConvert(m_stream.get(), &header, 
                             ACM_STREAMCONVERTF_BLOCKALIGN))
            throw std::runtime_error("decoding failed");
        if (acmStreamUnprepareHeader(m_stream.get(), &header, 0))
            throw std::runtime_error("acmStreamUnprepareHeader() failed");

        unsigned chanmask = m_channel_mask;
        if (!chanmask)
            chanmask = audio_chunk::g_guess_channel_config(m_ofmt.nChannels);
        p_chunk.set_data_fixedpoint(m_sample_buffer.data(),
                                    header.cbDstLengthUsed,
                                    m_ofmt.nSamplesPerSec,
                                    m_ofmt.nChannels,
                                    m_ofmt.wBitsPerSample,
                                    chanmask);
    }
    void decode(const void *p_buffer, t_size p_bytes, audio_chunk &p_chunk,
                abort_callback &p_abort)
    {
        if (is_lpcm())
            decode_lpcm(p_buffer, p_bytes, p_chunk, p_abort);
        else
            decode_acm(p_buffer, p_bytes, p_chunk, p_abort);
    }
    t_size set_stream_property(const GUID &p_type, t_size p_param1,
                               const void *p_param2, t_size p_param2size)
    {
        return 0;
    }
    unsigned get_max_frame_dependency()
    {
        return 0;
    }
    double get_max_frame_dependency_time()
    {
        return 0.0;
    }
    void reset_after_seek()
    {
    }
    bool analyze_first_frame_supported()
    {
        return false;
    }
    virtual void analyze_first_frame(const void *p_buffer, t_size p_bytes,
                                     abort_callback &p_abort)
    {
    }
};

static packet_decoder_factory_t<acm_packet_decoder>
    g_acm_packet_decoder_factory;

DECLARE_COMPONENT_VERSION(
    "ACM Packet Decoder",
    "0.0.2",
    "ACM Packet Decoder, Copyright (c) 2016 nu774"
);
VALIDATE_COMPONENT_FILENAME("foo_acm_packet_decoder.dll");
