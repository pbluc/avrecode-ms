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
  // Representation invariant: XXX(fill in)
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
      // Find the number in [low, low+range) which divides the largest 2^k and ends with a 1 bit.
      for (FixedPoint stop_bit = fixed_one; stop_bit > 0; stop_bit >>= 1) {
        FixedPoint x = (low | stop_bit) & ~(stop_bit - 1);
        if (low <= x && x < low + range) {
          low = x;
          break;
        }
      }
      range = 0;
      while (low != 0) {
        renormalize_and_emit_digit<OutputDigit>();
      }
    }

    void finish_cabac() {
      // Special-case for CABAC, which has a special end sequence. The way
      // CABAC generates the stop bit doesn't seem to be entirely correct for
      // arithmetic coding (the lower bound can decrease!) but I guess it works
      // with the CABAC tables? Or breaks rarely enough to not matter?
      FixedPoint stop_bit = (1 << 7);
      while (range >= (1 << 9)) {
        range >>= 1;
        stop_bit <<= 1;
      }
      low = (low | stop_bit) & ~(stop_bit - 1);
      range = 0;
      // Another CABAC special case: omit the last digit when it's only a stop bit.
      while (low != 0 && low != fixed_one/2) {
        renormalize_and_emit_digit<OutputDigit>();
      }
    }

   private:
    template <typename Digit>
    void renormalize_and_emit_digit() {
      static constexpr FixedPoint digit_base = FixedPoint(std::numeric_limits<Digit>::max()) + 1;
      static constexpr FixedPoint most_significant_digit = fixed_one / digit_base;

      Digit digit = low / most_significant_digit;
      if (digit != (low + range) / most_significant_digit) {
        // XXX
#if 0
        throw std::runtime_error("overflow not implemented");
#endif
      }
      low -= digit * most_significant_digit;
      low *= digit_base;
      range *= digit_base;
      emit_digit(digit);
    }

    template <typename Digit>
    void emit_digit(Digit digit) {
      for (int i = sizeof(Digit)-sizeof(OutputDigit); i >= 0; i -= sizeof(OutputDigit)) {
        *out++ = OutputDigit(digit >> (8*i));
      }
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
  return typename Coder::template encoder<decltype(it), typename OutputContainer::value_type>(it);
}
