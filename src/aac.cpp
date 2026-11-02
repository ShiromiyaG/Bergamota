#include "runtime/aac.h"

#ifdef RUNTIME_AAC
#include <aacdecoder_lib.h>
#endif

#include <algorithm>
#include <cstdio>

namespace runtime {

#ifdef RUNTIME_AAC

struct AacDecoder::State {
    HANDLE_AACDECODER decoder = nullptr;
};

AacDecoder::AacDecoder() : state_(std::make_unique<State>()) {}

AacDecoder::~AacDecoder() { reset(); }

bool AacDecoder::available() { return true; }

void AacDecoder::reset() {
    if (state_->decoder) aacDecoder_Close(state_->decoder);
    state_->decoder = nullptr;
}

bool AacDecoder::decode(const uint8_t *adts, size_t size, std::vector<int16_t> channels[2], uint32_t &rate) {
    channels[0].clear(), channels[1].clear();
    // the decoder reads ADTS itself: the headers, the frames' lengths
    if (!state_->decoder && !(state_->decoder = aacDecoder_Open(TT_MP4_ADTS, 1))) return false;
    // out as two channels at most, a stereo track's downmix otherwise
    aacDecoder_SetParam(state_->decoder, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);
    HANDLE_AACDECODER decoder = state_->decoder;
    std::vector<INT_PCM> pcm(2048 * 8);
    size_t at = 0;
    bool any = false;
    while (true) {
        // what is left fed in (the decoder takes as much as its buffer holds)
        if (at < size) {
            UCHAR *buffer = const_cast<UCHAR *>(adts + at);
            UINT length = UINT(size - at), valid = length;
            if (aacDecoder_Fill(decoder, &buffer, &length, &valid) != AAC_DEC_OK) break;
            at += length - valid;
        }
        AAC_DECODER_ERROR error = aacDecoder_DecodeFrame(decoder, pcm.data(), INT(pcm.size()), 0);
        if (error == AAC_DEC_NOT_ENOUGH_BITS) {
            if (at >= size) break; // all of it decoded
            continue;
        }
        if (error != AAC_DEC_OK) {
            // a frame that would not decode: the next one may
            static int told = 0;
            if (told++ < 8) std::fprintf(stderr, "aac: a frame did not decode, 0x%X\n", unsigned(error));
            if (at >= size) break;
            continue;
        }
        const CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
        if (!info || info->numChannels <= 0 || info->frameSize <= 0) continue;
        rate = uint32_t(info->sampleRate);
        const int count = std::min(info->numChannels, 2);
        for (int c = 0; c < count; c++)
            for (int i = 0; i < info->frameSize; i++) channels[c].push_back(int16_t(pcm[size_t(i) * info->numChannels + c]));
        any = true;
    }
    return any;
}

#else

struct AacDecoder::State {};
AacDecoder::AacDecoder() : state_(std::make_unique<State>()) {}
AacDecoder::~AacDecoder() = default;
bool AacDecoder::available() { return false; }
void AacDecoder::reset() {}
bool AacDecoder::decode(const uint8_t *, size_t, std::vector<int16_t> channels[2], uint32_t &) {
    channels[0].clear(), channels[1].clear();
    return false;
}

#endif

} // namespace runtime
