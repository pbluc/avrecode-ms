//
// Generic arithmetic coding. Used both for recoded encoding/decoding and for
// CABAC re-encoding.
//

#pragma once

#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>


template <typename FixedPoint = uint32_t, typename CompressedDigit = uint16_t, int MinRange = 0>
struct arithmetic_code {
  static_assert(std::numeric_limits<FixedPoint>::is_exact, "integer types only");
  static_assert(std::numeric_limits<CompressedDigit>::is_exact, "integer types only");

  // The representation of 1.0 in fixed-point, e.g. 0x80000000 for uint32_t.
  static constexpr FixedPoint fixed_one = std::numeric_limits<FixedPoint>::max()/2 + 1;
  // The base for compressed digit outputs, e.g. 0x10000 for uint16_t.
  static constexpr FixedPoint compressed_digit_base = FixedPoint(std::numeric_limits<CompressedDigit>::max()) + 1;
  // 1/base, representated in fixed point. Used to extract the current first digit.
  static constexpr FixedPoint most_significant_digit = fixed_one / compressed_digit_base;
  // The minimum precision for probability estimates, e.g. 0x100 for 8-bit
  // probabilities as in CABAC.  There is a space-time tradeoff: less precision
  // means poorer compression, but more precision causes overflow digits more often.
  static constexpr FixedPoint min_range = MinRange > 0 ? MinRange : (fixed_one/compressed_digit_base) / 16;

  static_assert(min_range > 1, "min_range too small");
  static_assert(min_range < fixed_one / compressed_digit_base, "min_range too large");

  // The encoder object takes an output iterator (e.g. to vector or ostream) to emit compressed digits.
  // Representation invariant: XXX(fill in)
  template <typename OutputIterator>
  class encoder {
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
      renormalize_and_emit_digits();
    }

    void finish() {
      range = 0;
      while (low != 0) {
        emit_digit();
      }
      range = fixed_one;
    }

   private:
    void renormalize_and_emit_digits() {
      while (range < min_range) {
        emit_digit();
      }
    }

    void emit_digit() {
      CompressedDigit digit = low / most_significant_digit;
      if (digit != (low + range) / most_significant_digit) {
        throw std::runtime_error("overflow not implemented");
      }
      low -= digit * most_significant_digit;
      low *= compressed_digit_base;
      range *= compressed_digit_base;
      *out++ = digit;
    }

    OutputIterator out;
    FixedPoint low = 0;
    FixedPoint range = fixed_one;
  };

  class decoder {
  };
};


template <typename Coder = arithmetic_code<>, typename OutputContainer>
typename Coder::template encoder<std::back_insert_iterator<OutputContainer>>
make_encoder(OutputContainer* container) {
  auto it = std::back_inserter(*container);
  return typename Coder::template encoder<decltype(it)>(it);
}
