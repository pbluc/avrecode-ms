#include <cstdlib>
#include <iostream>
#include <vector>

#include "arithmetic_code.h"
#include "cabac_code.h"

extern "C" {
#include "libavcodec/cabac.h"
}


int main(int argc, char* argv[]) {
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
  std::vector<uint16_t> out;
  auto encoder = make_encoder<cabac>(&out);

  for (int i = 0; i < bits.size(); i++) {
    encoder.put(bits[i], &states[contexts[i]]);
  }
  encoder.put_terminate(true);

  std::vector<uint8_t> compressed_bytes;
  for (uint16_t word : out) {
    compressed_bytes.push_back((word >> 8) & 0xFF);
    compressed_bytes.push_back(word & 0xFF);
  }

  std::cout << "compressed size: " << compressed_bytes.size() << std::endl;

  states.resize(0);
  states.resize(0x400);

  CABACContext ctx;
  ff_init_cabac_decoder(&ctx, &compressed_bytes[0], compressed_bytes.size(), nullptr);
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
}
