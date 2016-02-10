#include <cstdlib>
#include <iostream>
#include <vector>

#include "arithmetic_code.h"
#include "cabac_code.h"

extern "C" {
#include "libavcodec/cabac.h"
}


int main(int argc, char* argv[]) {
#if 0
  // Testing a particular input that triggered a CABAC encoder bug.
  std::vector<int> states = {15, 17, 106, 28, 16, 0, 10, 26, 33, 22, 35, 58, 44, 0, 0, 1, 3, 5};
  std::vector<int> bits   = { 1,  0,   0,  0,  0, 0,  0,  1,  1,  1,  1,  0,  1, 1, 1, 1, 1, 1};

  std::vector<uint8_t> out;
  cabac::encoder<std::back_insert_iterator<std::vector<uint8_t>>> encoder(std::back_inserter(out));
  uint8_t state;

  state = states[0];
  encoder.put(bits[0], &state);
  encoder.put_terminate(false);
  for (int i = 1; i < bits.size(); i++) {
    state = states[i];
    encoder.put(bits[i], &state);
  }
  for (int i = 0; i < 16; i++) {
    state = 0;
    encoder.put(0, &state);
  }
  encoder.put_terminate(true);

  std::cout << out.size() << " " << (unsigned int)out[0] << " " << (unsigned int)out[1] << std::endl;
  CABACContext ctx;
  ff_init_cabac_decoder(&ctx, &out[0], out.size(), nullptr);
  state = states[0];
  ff_get_cabac(&ctx, &state);
  ff_get_cabac_terminate(&ctx);
  for (int i = 1; i < bits.size(); i++) {
    state = states[i];
    std::cout << (ff_get_cabac(&ctx, &state)==bits[i]) << std::endl;
  }

  return 0;
#else
  std::srand(time(nullptr));

  std::vector<int> probabilities;
  for (int i = 0; i < 5; i++) {
    probabilities.push_back(std::rand() % 100);
  }

  std::vector<int> bits;
  std::vector<int> contexts;
  for (int i = 0; i < std::stoi(argv[1]); i++) {
    int context = std::rand() % probabilities.size();
    contexts.push_back(context);
    bits.push_back((std::rand() % 100) > probabilities[context]);
  }

  std::vector<uint8_t> states(0x400);
  std::vector<uint8_t> out;
#if 0
  cabac::encoder<std::back_insert_iterator<std::vector<uint8_t>>> encoder(std::back_inserter(out));

  for (int i = 0; i < bits.size(); i++) {
    encoder.put(bits[i], &states[contexts[i]]);
  }
  encoder.put_terminate(true);

  std::cout << "compressed size: " << out.size() << std::endl;

  states.resize(0);
  states.resize(0x400);

  CABACContext ctx;
  ff_init_cabac_decoder(&ctx, &out[0], out.size(), nullptr);
  for (int i = 0; i < bits.size(); i++) {
    int bit = ff_get_cabac(&ctx, &states[contexts[i]]);
    if (bit != bits[i]) {
      std::cerr << "mismatch at bit: " << i << ", " << bit << " != " << bits[i] << std::endl;
      return 1;
    }
  }
  if (!ff_get_cabac_terminate(&ctx)) {
    std::cerr << "mismatch at terminate" << std::endl;
  }
  return 0;
#else
  typedef arithmetic_code<uint64_t, uint16_t> code;
  auto encoder = make_encoder<code>(&out);

  for (int i = 0; i < bits.size(); i++) {
    encoder.put(bits[i], [](uint64_t range){ return range/2; });
  }
  encoder.finish();

  std::cout << "compressed size: " << out.size() << std::endl;

  auto decoder = make_decoder<code>(out);
  for (int i = 0; i < bits.size(); i++) {
    int bit = decoder.get([](uint64_t range){ return range/2; });
    if (bit != bits[i]) {
      std::cerr << "mismatch at bit: " << i << ", " << bit << " != " << bits[i] << std::endl;
      return 1;
    }
  }
  return 0;
#endif
#endif
}
