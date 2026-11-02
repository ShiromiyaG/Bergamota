// The audio firmware's AAC decoder, which titles drive through the DSP's
// binary pipe (Pokémon X and Y, Omega Ruby and Alpha Sapphire play their
// music through it): ADTS frames in, each channel's 16-bit PCM out. Decoded
// by Fraunhofer's FDK AAC (fetched by CMake; its NOTICE goes with the
// package), kept between requests, since each frame overlaps the one before.
#ifndef RUNTIME_AAC_H
#define RUNTIME_AAC_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace runtime {

class AacDecoder {
public:
    AacDecoder();
    ~AacDecoder();
    AacDecoder(const AacDecoder &) = delete;
    AacDecoder &operator=(const AacDecoder &) = delete;

    // whether there is a decoder at all (the runtime built with it)
    static bool available();

    // the frames in adts decoded, each channel's samples in channels (at
    // most two); the stream's rate. false when nothing decoded
    bool decode(const uint8_t *adts, size_t size, std::vector<int16_t> channels[2], uint32_t &rate);

    // a new stream: the overlap of the one before forgotten
    void reset();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace runtime

#endif
