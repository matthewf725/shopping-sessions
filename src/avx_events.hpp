#pragma once
#if defined(__AVX512BW__) && defined(__AVX512DQ__) && defined(__AVX512VL__)
namespace shop {
template <class Scalar, class Batch> struct EventProjection {
    Scalar scalar;
    Batch batch;
    template <class... Args> void operator()(Args&&... args) const {
        scalar(std::forward<Args>(args)...);
    }
};

struct EventLanes {
    using V = __m512i;
    using I = __m256i;
    using M = __mmask8;
    static V number(uint64_t value) {
        return _mm512_set1_epi64(value);
    }
    static I integer(uint32_t value) {
        return _mm256_set1_epi32(value);
    }
    static V load8(const char* data, I index) {
        return _mm512_i32gather_epi64(index, data, 1);
    }
    static V lookup(const void* base, I indices) {
        return _mm512_i32gather_epi64(indices, base, 8);
    }
    struct Field {
        I start, size;
        V head;
    };
    static Field field(const char* data, I before, I after) {
        I raw = _mm256_add_epi32(before, integer(1));
        V head = load8(data, raw);
        M quoted = _mm512_cmpeq_epi64_mask(head & number(255), number('"'));
        I delta = _mm256_maskz_set1_epi32(quoted, 1);
        return {_mm256_add_epi32(raw, delta),
                _mm256_sub_epi32(_mm256_sub_epi32(after, raw), _mm256_add_epi32(delta, delta)),
                _mm512_mask_srli_epi64(head, quoted, head, 8)};
    }
    static V keys(const char* data, Field f, char prefix, M& good) {
        V words = load8(data + 1, f.start);
        const V reverse = _mm512_set4_epi32(0x08090a0b, 0x0c0d0e0f, 0x00010203, 0x04050607);
        words = _mm512_shuffle_epi8(words, reverse);
        I size = _mm256_sub_epi32(integer(9), f.size);
        V shift = _mm512_slli_epi64(_mm512_cvtepu32_epi64(size), 3);
        good &= _mm256_cmp_epu32_mask(size, integer(7), _MM_CMPINT_LE);
        good &= _mm512_cmpeq_epi64_mask(f.head & number(255), number(prefix));
        good &= _mm512_cmp_epi64_mask(words, number(0), _MM_CMPINT_GE);
        good &= _mm512_test_epi64_mask(_mm512_srlv_epi64(words, shift), number(255));
        return words & _mm512_sllv_epi64(number(UINT64_MAX), shift);
    }
    static V clock_order(V hms) {
        V nibs = hms & number(0x0f0f000f0f000f0fULL);
        V pairs = _mm512_slli_epi64(nibs, 4) | _mm512_srli_epi64(nibs, 8);
        // Each eight-byte lane contains hour at byte 0, minute at 3, second at 6.
        const V gather = _mm512_set4_epi32(0x80808080, 0x80080b0e, 0x80808080, 0x80000306);
        return _mm512_shuffle_epi8(pairs, gather);
    }
    static V seconds(V ascii, M& valid) {
        V d = ascii ^ number(0x30303a30303a3030ULL);
        V errors = (d & number(~0x0f0f000f0f000f0fULL)) |
                   (_mm512_add_epi64(d, number(0x060a00060a00060dULL)) & number(0x1010001010001010ULL));
        V pairs = _mm512_add_epi64(_mm512_add_epi64(_mm512_slli_epi64(d, 3), _mm512_slli_epi64(d, 1)),
                                   _mm512_srli_epi64(d, 8)) &
                  number(0x00ff0000ff0000ffULL);
        V h = pairs & number(255);
        V m = _mm512_srli_epi64(pairs, 24) & number(255);
        V s = _mm512_srli_epi64(pairs, 48);
        valid &= _mm512_cmpeq_epi64_mask(errors, number(0));
        valid &= _mm512_cmp_epu64_mask(h, number(23), _MM_CMPINT_LE);
        return _mm512_add_epi64(_mm512_mul_epu32(h, number(3600)),
                                _mm512_add_epi64(_mm512_mul_epu32(m, number(60)), s));
    }
    static V decimal8(V ascii, M& valid) {
        V digits = _mm512_sub_epi8(ascii, _mm512_set1_epi8('0'));
        V invalid = _mm512_subs_epu8(digits, _mm512_set1_epi8(9));
        valid &= _mm512_cmpeq_epi64_mask(invalid, number(0));
        V pairs = _mm512_maddubs_epi16(digits, _mm512_set1_epi16(0x010a));
        V groups = _mm512_madd_epi16(pairs, _mm512_set1_epi32(0x00010064));
        return _mm512_add_epi64(_mm512_mul_epu32(groups, number(10000)), _mm512_srli_epi64(groups, 32));
    }
    template <class Store>
    static M run(const Window& window, const char* data, const uint32_t* cuts, const uint16_t* rows,
                 Store store) {
        I indices = _mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(rows)));
        I boundaries[6];
        for (unsigned i = 0; i < 6; ++i)
            boundaries[i] = _mm256_i32gather_epi32(reinterpret_cast<const int*>(cuts + i), indices, 4);
        Field id = field(data, boundaries[0], boundaries[1]),
              user = field(data, boundaries[1], boundaries[2]);
        Field time = field(data, boundaries[2], boundaries[3]),
              ingest = field(data, boundaries[3], boundaries[4]);
        Field kind = field(data, boundaries[4], boundaries[5]);
        M good = _mm256_cmpeq_epi32_mask(ingest.size, integer(20)) &
                 _mm256_cmpeq_epi32_mask(kind.size, integer(4));
        V type = kind.head & number(UINT32_MAX);
        good &= _mm512_cmpeq_epi64_mask(type, number(0x77656976)) |
                _mm512_cmpeq_epi64_mask(type, number(0x74726163));
        V product = load8(data + 1, boundaries[5]);
        good &= ~(_mm512_cmpeq_epi64_mask(product & number(255), number(',')) |
                  _mm512_cmpeq_epi64_mask(product & number(65535), number(0x2222)));
        V event_key = keys(data, id, 'e', good), person = keys(data, user, 'u', good);
        V ingest_month = load8(data, ingest.start), ingest_day = load8(data + 8, ingest.start);
        V slot =
            _mm512_slli_epi64(ingest_day & number(3), 4) | (_mm512_srli_epi64(ingest_day, 8) & number(15));
        I ci = _mm512_cvtepi64_epi32(_mm512_add_epi64(slot, _mm512_slli_epi64(slot, 1)));
        static_assert(sizeof(Window::IngestDay) == 24 && sizeof(Window::EpochPrefix) == 16);
        const auto* days = window.ingest_days_.data();
        good &= _mm512_cmpeq_epi64_mask(ingest_month, lookup(&days->month_prefix, ci));
        V midnight = lookup(&days->midnight, ci);
        V order =
            _mm512_add_epi64(lookup(&days->order_base, ci), clock_order(load8(data + 11, ingest.start)));
        good &= _mm512_cmp_epu64_mask(order, number(0x80000000u), _MM_CMPINT_LT);
        V time_head = load8(data, time.start);
        M iso = _mm256_cmpeq_epi32_mask(time.size, integer(20));
        iso &= _mm512_cmpeq_epi64_mask(time_head, ingest_month);
        iso &=
            _mm512_cmpeq_epi64_mask((load8(data + 8, time.start) ^ ingest_day) & number(0xffffff), number(0));
        iso &= _mm512_cmpeq_epi64_mask(load8(data + 19, time.start) & number(255), number('Z'));
        V iso_secs = seconds(load8(data + 11, time.start), iso);
        V iso_ts = _mm512_add_epi64(midnight, _mm512_mul_epu32(iso_secs, number(1000)));
        M epoch = _mm256_cmpeq_epi32_mask(time.size, integer(13));
        V eslot = _mm512_slli_epi64(_mm512_srli_epi64(time_head, 24) & number(3), 4) |
                  (_mm512_srli_epi64(time_head, 32) & number(15));
        I ei = _mm512_cvtepi64_epi32(_mm512_slli_epi64(eslot, 1));
        const auto* prefixes = window.epoch_prefixes_.data();
        V prefix = lookup(&prefixes->text, ei);
        epoch &= _mm512_cmpeq_epi64_mask(prefix, time_head & number(0xffffffffffULL)) &
                 _mm512_cmpneq_epi64_mask(prefix, number(0));
        V epoch_ts =
            _mm512_add_epi64(lookup(&prefixes->base, ei), decimal8(load8(data + 5, time.start), epoch));
        good &= iso | epoch;
        V ts = _mm512_mask_blend_epi64(iso, epoch_ts, iso_ts);
        good &= _mm512_cmp_epi64_mask(ts, midnight, _MM_CMPINT_GE) &
                _mm512_cmp_epi64_mask(ts, number(window.end), _MM_CMPINT_LT);
        V when = _mm512_sub_epi64(ts, number(window.start));
        good &= _mm512_cmp_epu64_mask(when, number(UINT32_MAX), _MM_CMPINT_LE);
        V hk = event_key ^ _mm512_srli_epi64(event_key, 32);
        hk = _mm512_mullo_epi64(hk, number(0xd6e8feb86659fd93ULL));
        hk ^= _mm512_srli_epi64(hk, 32);
        alignas(64) uint64_t ids[8], people[8], values[8];
        _mm512_store_si512(ids, hk);
        _mm512_store_si512(people, person);
        _mm512_store_si512(values, _mm512_slli_epi64(order, 32) | when);
        for (unsigned bits = good; bits; bits &= bits - 1) {
            unsigned i = std::countr_zero(bits);
            store(ids[i], people[i], values[i]);
        }
        return good;
    }
};
} // namespace shop
#endif
