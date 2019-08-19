/*
 * Copyright 2017-2019 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AVS_COMMONS_ALGORITHM_BASE64_H
#define AVS_COMMONS_ALGORITHM_BASE64_H

#include <avsystem/commons/defs.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file base64.h
 *
 * Base64 encoder / decoder.
 */

/**
 * Array of characters for standard base64 encoder, i.e. AVS_BASE64_CHARS[0]
 * through AVS_BASE64_CHARS[63] are the characters that encode corresponding
 * numbers.
 */
extern const char AVS_BASE64_CHARS[];

/**
 * Array of characters for alternate base64 alphabet, as defined by RFC 4648
 * Section 5.
 */
extern const char AVS_BASE64_URL_SAFE_CHARS[];

/**
 * Returns amount of bytes required to store input encoded in base64 if padding
 * is used.
 *
 * @param input_length Length of input in bytes.
 *
 * @returns length of base64 encoded input of length @p input_length.
 */
size_t avs_base64_encoded_size(size_t input_length);

/**
 * Returns amount of bytes required to store input encoded in base64 if padding
 * is NOT used.
 *
 * @param input_length Length of input in bytes.
 *
 * @returns length of base64 encoded input of length @p input_length.
 */
size_t avs_base64_encoded_size_without_padding(size_t input_length);

/**
 * Returns amount of bytes that would be sufficient to store input encoded from
 * base64.
 *
 * Warning: this function computes just a rough estimate of amount of bytes that
 * are actually necessary by taking 3 * (input_length / 4) as a approximation.
 * Therefore the returned value is always an upper bound.
 *
 * @param input_length  Length of base64 encoded input.
 *
 * @returns estimate of the length of buffer required to store decoded input.
 */
size_t avs_base64_estimate_decoded_size(size_t input_length);

/**
 * Encodes specified input into a custom variant of base64.
 *
 * Note: this function fails if @p out_length is too small to encode @p input,
 * to predict buffer requirements use @ref avs_base64_encoded_size .
 *
 * @param out           Pointer to user-allocated array where encoded data will
 *                      be written.
 * @param out_length    Length of user-allocated array.
 * @param input         Input to encode.
 * @param input_length  Length of the input.
 * @param alphabet      Array of characters to use for encoding; values from
 *                      alphabet[0] through alphabet[63], inclusive, MUST be
 *                      accessible and unique.
 * @param padding_char  Character to use for final padding. If '\0' is passed,
 *                      no padding is used.
 *
 * @returns 0 on success, negative value in case of error.
 */
int avs_base64_encode_custom(char *out,
                             size_t out_length,
                             const uint8_t *input,
                             size_t input_length,
                             const char *alphabet,
                             char padding_char);

/**
 * Encodes specified input into base64.
 *
 * Note: this function fails if @p out_length is too small to encode @p input,
 * to predict buffer requirements use @ref avs_base64_encoded_size .
 *
 * @param out           Pointer to user-allocated array where encoded data will
 *                      be written.
 * @param out_length    Length of user-allocated array.
 * @param input         Input to encode.
 * @param input_length  Length of the input.
 *
 * @returns 0 on success, negative value in case of error.
 */
static inline int avs_base64_encode(char *out,
                                    size_t out_length,
                                    const uint8_t *input,
                                    size_t input_length) {
    return avs_base64_encode_custom(
            out, out_length, input, input_length, AVS_BASE64_CHARS, '=');
}

/**
 * Decodes specified input in a custom base64 format.
 *
 * Note that this function fails if @p out_length is too small. To predict
 * buffer requirements use @ref avs_base64_estimate_decoded_size .
 *
 * @param out              Pointer to user-allocated array where decoded data
 *                         will be stored.
 * @param out_length       Length of user-allocated array.
 * @param input            Null terminated input to decode.
 * @param alphabet         Array of characters to use for encoding; values from
 *                         alphabet[0] through alphabet[63], inclusive, MUST be
 *                         accessible and unique.
 * @param padding_char     Character to expect as final padding. If '\0' is
 *                         passed, no padding is expected.
 * @param allow_whitespace If true, whitespace characters in the middle of input
 *                         are allowed and ignored. If false, any whitespace
 *                         character will cause an error.
 * @param require_padding  If @p padding_char is not '\0' and this flag is true,
 *                         the input (stripped of any whitespace characters if
 *                         @p allow_whitespace is true) is required to have a
 *                         length that is a multiple of four, and end with
 *                         appropriate number of padding characters. Otherwise,
 *                         the padding character is ignored. Note that the value
 *                         of this flag has no effect if @p padding_char is
 *                         '\0'.
 *
 * @returns length of decoded data in bytes, negative value in case of error.
 */
ssize_t avs_base64_decode_custom(uint8_t *out,
                                 size_t out_length,
                                 const char *input,
                                 const char *alphabet,
                                 char padding_char,
                                 bool allow_whitespace,
                                 bool require_padding);

/**
 * Decodes specified base64 input.
 *
 * Note:
 * 1. It does not accept inputs with whitespace characters of any kind.
 * 2. It does not accept inputs with with superflous padding characters.
 * 3. It does not accept inputs that are not padded properly.
 * 4. As a consequence it does not accepts inputs whose length is not a multiple
 *    of four.
 *
 * Moreover, this function fails if @p out_length is too small. To predict
 * buffer requirements use @ref avs_base64_estimate_decoded_size (which, for
 * inputs accepted by this function will return the exact amount of bytes
 * needed).
 *
 * @param out           Pointer to user-allocated array where decoded data
 *                      will be stored.
 * @param out_length    Length of user-allocated array.
 * @param input         Null terminated input to decode.
 *
 * @returns length of decoded data in bytes, negative value in case of error.
 */
static inline ssize_t
avs_base64_decode_strict(uint8_t *out, size_t out_length, const char *input) {
    return avs_base64_decode_custom(
            out, out_length, input, AVS_BASE64_CHARS, '=', false, true);
}

/**
 * Does the same as @ref avs_base64_decode_strict except that it ignores
 * superflous whitespaces and padding characters.
 *
 * Note that this function fails if @p out_length is too small. To predict
 * buffer requirements use @ref avs_base64_estimate_decoded_size .
 *
 * @param out           Pointer to user-allocated array where decoded data will
 *                      be stored.
 * @param out_length    Length of user-allocated array.
 * @param input         Null terminated input to decode.
 *
 * @returns length of decoded data in bytes, negative value in case of error.
 */
static inline ssize_t
avs_base64_decode(uint8_t *out, size_t out_length, const char *input) {
    return avs_base64_decode_custom(
            out, out_length, input, AVS_BASE64_CHARS, '=', true, false);
}

#ifdef __cplusplus
}
#endif

#endif /* AVS_COMMONS_ALGORITHM_BASE64_H */
