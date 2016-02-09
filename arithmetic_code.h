//
// Generic arithmetic coding. Used both for recoded encoding/decoding and for
// CABAC re-encoding.
//
// Some notes on the data representations used by the encoder and decoder.
// Uncompressed data:
//   Symbols: b_1 ... b_n \in {0,1} .
//   Probabilities: p_1 ... p_n \in [0,1], where p_i estimates the probability that b_i=1.
// Compressed data:
//   Arithmetic coding represents a compressed stream of symbols as an
//   arbitrary-precision number C \in [0,1] .
//   If the compressed digits in base M are c_k \in {0..M-1}, then
//   C = \sum_{k=1}^K c_k M^{-k} .
//   Arithmetic coding uses the probabilities p_i to link the symbols b_i with
//   the compressed digits c_k:
//   C_i = (1-p_i) b_i + p_i C_{i+1} (1-b_i)  \in [0,1]
//   C_1 = C = \sum_{k=1}^K c_k M^{-k}
//   C_n is an arbitrary value in [0,1]
// Intermediate representation state:
//   Maximum R (any positive number, typically 2^k)
//   Lower and upper bounds x, y \in [0,R)
//   Range r = y-x \in [0,R)
// Representation invariant:
//   C = \sum_{k=1}^{K_i} c_k M^{-k} + M^{-K_i} ( x_i + r_i C_i ) / R_i
//   Base case: K_1 = 0, x_1 = 0, r_1 = R_1
// The various encoding and decoding methods modify K, x, r, R while keeping C fixed.
//

#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>


template <typename FixedPoint = uint32_t, typename CompressedDigit = uint16_t, int MinRange = 0>
struct arithmetic_code {
  static_assert(std::numeric_limits<FixedPoint>::is_exact, "integer types only");
  static_assert(std::numeric_limits<CompressedDigit>::is_exact, "integer types only");
  static_assert(!std::numeric_limits<FixedPoint>::is_signed, "unsigned integer types only");
  static_assert(!std::numeric_limits<CompressedDigit>::is_signed, "unsigned integer types only");

  // The representation of 1.0 in fixed-point, e.g. 0x80000000 for uint32_t.
  static constexpr FixedPoint fixed_one = std::numeric_limits<FixedPoint>::max()/2 + 1;
  // The base for compressed digit outputs, e.g. 0x10000 for uint16_t.
  static constexpr FixedPoint compressed_digit_base = FixedPoint(std::numeric_limits<CompressedDigit>::max()) + 1;
  // The minimum precision for probability estimates, e.g. 0x100 for 8-bit
  // probabilities as in CABAC.  There is a space-time tradeoff: less precision
  // means poorer compression, but more precision causes overflow digits more often.
  static constexpr FixedPoint min_range = MinRange > 0 ? MinRange : (fixed_one/compressed_digit_base) / 16;

  static_assert(min_range > 1, "min_range too small");
  static_assert(min_range < fixed_one/compressed_digit_base, "min_range too large");

  // The encoder object takes an output iterator (e.g. to vector or ostream) to emit compressed digits.
  template <typename OutputIterator,
            typename OutputDigit = typename std::iterator_traits<OutputIterator>::value_type>
  class encoder {
    static_assert(std::numeric_limits<OutputDigit>::is_exact, "integer types only");
    static_assert(!std::numeric_limits<OutputDigit>::is_signed, "unsigned integer types only");
    static_assert(sizeof(CompressedDigit) % sizeof(OutputDigit) == 0, "size of compressed digit must be a multiple of size of output digit");

   public:
    explicit encoder(OutputIterator out) : out(out) {}
    encoder(OutputIterator out, FixedPoint initial_range) : out(out), range(initial_range) {}
    ~encoder() { finish(); }

    void put(int symbol, std::function<FixedPoint(FixedPoint)> probability_of_1) {
      FixedPoint range_of_1 = probability_of_1(range);
      FixedPoint range_of_0 = range - range_of_1;
      if (symbol != 0) {
        low += range_of_0;
        range = range_of_1;
      } else {
        range = range_of_0;
      }
      while (range < min_range) {
        renormalize_and_emit_digit<CompressedDigit>();
      }
    }

    void finish() {
      // Find largest stop bit 2^k < range, and x such that 2^k divides x,
      // 2^{k+1} doesn't divide x, and x is in [low, low+range).
      for (FixedPoint stop_bit = (fixed_one >> 1); stop_bit > 0; stop_bit >>= 1) {
        FixedPoint x = (low | stop_bit) & ~(stop_bit - 1);
        if (stop_bit < range && low <= x && x < low + range) {
          low = x;
          break;
        }
      }

      range = 1;
      while (low != 0) {
        renormalize_and_emit_digit<OutputDigit>();
      }
    }

   private:
    template <typename Digit>
    void renormalize_and_emit_digit() {
      static constexpr FixedPoint digit_base = FixedPoint(std::numeric_limits<Digit>::max()) + 1;
      static constexpr FixedPoint most_significant_digit = fixed_one / digit_base;

      // Check for a carry bit, and cascade from lowest overflow digit to highest.
      if (low >= fixed_one) {
        for (int i = overflow.size()-1; i >= 0; i--) {
          if (++overflow[i] != 0) break;
        }
        low -= fixed_one;
      }
      assert(low < fixed_one);

      // Compare the minimum and maximum possible values of the top digit.
      // If different, defer emitting the digit until we're sure we won't have to carry.
      Digit digit = Digit(low / most_significant_digit);
      if (digit != Digit((low + range - 1) / most_significant_digit)) {
        assert(range < most_significant_digit);
        overflow.push_back(digit);
      } else {
        for (CompressedDigit overflow_digit : overflow) {
          emit_digit(overflow_digit);
        }
        overflow.clear();
        emit_digit(digit);
      }

      // Subtract away the emitted/overflowed digit and renormalize.
      low -= digit * most_significant_digit;
      low *= digit_base;
      range *= digit_base;
    }

    // Emit a CompressedDigit as one or more OutputDigits. Loop should be unrolled by the compiler.
    template <typename Digit>
    void emit_digit(Digit digit) {
      for (int i = sizeof(Digit)-sizeof(OutputDigit); i >= 0; i -= sizeof(OutputDigit)) {
        *out++ = OutputDigit(digit >> (8*i));
      }
    }

    // Output digits are emitted to this iterator as they are produced.
    OutputIterator out;
    // The lower bound x. (When overflow.size() > 0, low is the fractional digits of x/R_0.)
    FixedPoint low = 0;
    // The range r, which starts as fixed-point 1.0.
    FixedPoint range = fixed_one;
    // High digits of x. If overflow.size() = z, then R = R_0 M^z (where R_0 = fixed_one).
    std::vector<CompressedDigit> overflow;
  };

  class decoder {
  };
};


template <typename Coder = arithmetic_code<>, typename OutputContainer>
typename Coder::template encoder<std::back_insert_iterator<OutputContainer>>
make_encoder(OutputContainer* container) {
  auto it = std::back_inserter(*container);
  return typename Coder::template encoder<decltype(it), typename OutputContainer::value_type>(it);
}
