/*
 * Copyright 2020 u-blox Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Only #includes of u_* and the C standard library are allowed here,
 * no platform stuff and no OS stuff.  Anything required from
 * the platform/OS must be brought in through u_port* to maintain
 * portability.
 */

/** @file
 * @brief Tests for the internal cellular chip to chip security API.
 * These should pass on all platforms.  No cellular module is
 * required to run this set of tests, all testing is back to back.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "string.h"    // memcpy(), memcmp()
#include "ctype.h"     // isdigit()

#include "u_cfg_sw.h"
#include "u_cfg_os_platform_specific.h"
#include "u_cfg_app_platform_specific.h"
#include "u_cfg_test_platform_specific.h"

#include "u_error_common.h"

#include "u_port.h"
#include "u_port_debug.h"
#include "u_port_os.h"
#include "u_port_uart.h"
#include "u_port_crypto.h"

#include "u_at_client.h"

#include "u_security.h"

#include "u_cell_module_type.h"
#include "u_cell.h"         // Order is
#include "u_cell_net.h"     // important here
#include "u_cell_private.h" // don't change it

#include "u_cell_sec_c2c.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/** The 16 byte TE secret to use during testing.
 */
#define U_CELL_SEC_C2C_TEST_TE_SECRET "\x00\x01\x02\x03\x04\x05\x06\x07" \
                                      "\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff"

/** The 16 byte key to use during testing.
 */
#define U_CELL_SEC_C2C_TEST_KEY "\x10\x11\x12\x13\x14\x15\x16\x17" \
                                "\xe8\xe9\xea\xeb\xec\xed\xee\xef"

/** The 16 byte truncated HMAC (or tag) to use
 * during testing, needed for V2 only.
 */
#define U_CELL_SEC_C2C_TEST_HMAC_TAG "\x20\x21\x22\x23\x24\x25\x26\x27" \
                                     "\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf"

/** We only send back what we receive so the max length
 * is the max TX length.
 */
#define U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES U_CELL_SEC_C2C_CHUNK_MAX_TX_LENGTH_BYTES

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/** Definition of clear text and encrypted version for back to back
 * testing of the intercept functions.
 */
typedef struct {
    bool isV2;
    const char *pTeSecret;
    const char *pKey;
    const char *pHmacTag; /** Needed for V2 only. */
    const char *pClear;
    size_t chunkLengthMax;
    size_t numChunks;
    // Allow up to five chunks for test purposes
    size_t clearLength[5];
    // Allow up to five chunks for test purposes
    size_t encryptedLength[5];
} uCellSecC2cTest_t;

#if (U_CFG_TEST_UART_A >= 0) && (U_CFG_TEST_UART_B >= 0)

/** Definition of an outgoing AT command, what the response
 * should be plus an optional URC, for testing of the intercept
 * functions inside the AT client.
 * ORDER IS IMPORTANT: this is statically initialised.
 */
typedef struct {
    bool isV2;
    size_t chunkLengthMax;
    const char *pTeSecret;
    const char *pKey;
    const char *pHmacTag; /** Needed for V2 only. */
    const char *pCommandPrefix;
    bool isBinary; /** Command and response are either
                       a string or binary bytes. */
    const char *pCommandBody;
    size_t commandBodyLength;
    const char *pUrcPrefix; /** Set to NULL if there is no URC. */
    const char *pUrcBody;  /** Can only be a string. */
    const char *pResponsePrefix;
    const char *pResponseBody;
    size_t responseBodyLength;
} uCellSecC2cTestAt_t;

#endif

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

/** Storage for the common part of the security context.
 */
static uCellSecC2cContext_t gContext;

/** Storage for the transmit/encode direction of the security context.
 */
static uCellSecC2cContextTx_t gContextTx;

/** Storage for the receive/decode direction of the security context.
 */
static uCellSecC2cContextRx_t gContextRx;

/** Test data.
 */
//lint -e{785} Suppress too few initialisers
//lint -e{786} Suppress string concatenation within initialiser
static uCellSecC2cTest_t gTestData[] = {
    {/* 1: Basic V1 */
        false, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "Hello world!", U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES, 1, {12},
        {1 + 2 + 12 + 4 /* pad to 16 */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1}
    },
    {/* 2: Basic V2 */
        true, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, U_CELL_SEC_C2C_TEST_HMAC_TAG,
        "Hello world!", U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES, 1, {12},
        {1 + 2 + 12 + 4 /* pad to 16 */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1}
    },
    {/* 3: V1, clear text exactly 16 bytes (padding length) long */
        false, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "0123456789abcdef", U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES, 1, {16},
        {1 + 2 + 32 /* padding causes this */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1}
    },
    {/* 4: V2, clear text exactly 16 bytes (padding length) long */
        true, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, U_CELL_SEC_C2C_TEST_HMAC_TAG,
        "0123456789abcdef", U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES, 1, {16},
        {1 + 2 + 32 /* padding causes this */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1}
    },
    {/* 5: V1, clear text of exactly chunk length when padded */
        false, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "47 bytes, one less than the chunk length of 48.", 48, 1, {47},
        {1 + 2 + 48 /* max chunk length when padded */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1}
    },
    {/* 6: V2, clear text of exactly chunk length when padded */
        true, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, U_CELL_SEC_C2C_TEST_HMAC_TAG,
        "47 bytes, one less than the chunk length of 48.", 48, 1, {47},
        {1 + 2 + 48 /* max chunk length when padded */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1}
    },
    {/* 7: V1, clear text of greater than the chunk length */
        false, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "With a chunk length of 48 this is just a bit longer at 58.", 48, 2, {47, 11},
        {
            1 + 2 + 48 /* max chunk length */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1,
            1 + 2 + 16 /* remainder, padded to 16 */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1
        }
    },
    {/* 8: V2, clear text of greater than the chunk length */
        true, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, U_CELL_SEC_C2C_TEST_HMAC_TAG,
        "With a chunk length of 48 this is just a bit longer at 58.", 48, 2, {47, 11},
        {
            1 + 2 + 48 /* max chunk length */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1,
            1 + 2 + 16 /* remainder, padded to 16 */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1
        }
    },
    {/* 9: V1, a biggee*/
        false, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789", 48, 5, {47, 47, 47, 47, 12},
        {
            1 + 2 + 48 /* max chunk length */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1,
            1 + 2 + 48 /* max chunk length */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1,
            1 + 2 + 48 /* max chunk length */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1,
            1 + 2 + 48 /* max chunk length */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1,
            1 + 2 + 16 /* remainder, padded to 16 */ + 32 /* SHA256 */ + 16  /* IV */ + 2 + 1
        }
    },
    {/* 10: V2, a biggee*/
        true, U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, U_CELL_SEC_C2C_TEST_HMAC_TAG,
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789", 48, 5, {47, 47, 47, 47, 12},
        {
            1 + 2 + 48 /* max chunk length */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1,
            1 + 2 + 48 /* max chunk length */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1,
            1 + 2 + 48 /* max chunk length */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1,
            1 + 2 + 48 /* max chunk length */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1,
            1 + 2 + 16 /* remainder, padded to 16 */ + 16  /* IV */ + 16 /* HMAC TAG */ + 2 + 1
        }
    }
};

/** A buffer for transmitted data.
 */
static char gBufferA[U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES * 5];

/** A buffer for received data.
 */
static char gBufferB[U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES * 5];

/** Handle for the AT client UART stream.
 */
static int32_t gUartAHandle = -1;

/** Handle for the AT server UART stream (i.e. the reverse direction).
 */
static int32_t gUartBHandle = -1;

#if (U_CFG_TEST_UART_A >= 0) && (U_CFG_TEST_UART_B >= 0)

/** A buffer for received URC data.
 */
static char gBufferC[U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES * 5];

/** For tracking heap lost to memory  lost by the C library.
 */
static size_t gSystemHeapLost = 0;

/** Count our way through the AT client-based tests.
 */
static size_t gAtTestCount = 0;

/** Flag an error on the server side of the AT interface.
 */
static int32_t gAtServerErrorOrSize = 0;

/** Flag an error in a URC.
 */
static int32_t gUrcErrorOrSize = 0;

/** Count the number of URCs received.
 */
static size_t gUrcCount = 0;

/** A chip-to-chip security context for the
 * AT server side.
 */
static uCellSecC2cContext_t gAtServerContext;

/** A receive chip-to-chip security context for the
 * AT server-side to use to decrypt packets.
 */
static uCellSecC2cContextRx_t gAtServerContextRx;

/** A transmit chip-to-chip security context for the
 * AT server-side to use to encrypt packets.
 */
static uCellSecC2cContextTx_t gAtServerContextTx;

/** Test data for the AT client based testing.
 */
//lint -e{786} Suppress string concatenation within initialiser
static const uCellSecC2cTestAt_t gTestAt[] = {
    {/* 1: command with string parameter and OK response, no URC */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+BLAH0=", false, "thing-thing", 11, /* Command with string parameter */
        NULL, NULL, /* No URC */
        NULL, NULL, 0 /* No prefix or response body */
    },
    {/* 2: command with string parameter and information response, no URC */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+BLAH1=", false, "thing thang", 11, /* Command with string parameter */
        NULL, NULL, /* No URC */
        "+BLAH1:", "thong", 5 /* Information response prefix and body  */
    },
    {/* 3: command with string parameter, URC inserted then OK response */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+BLAH2=", false, "whotsit", 7, /* Command with string parameter */
        "+UBOO:", "bang", /* URC inserted */
        NULL, NULL, 0 /* No prefix or response body */
    },
    {/* 4: command with string parameter, URC inserted then information response */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+BLAH3=", false, "questionable", 12, /* Command with string parameter */
        "+UPAF:", "boomer", /* URC inserted */
        "+BLAH3:", "not at all", 10 /* Information response prefix and body  */
    },
    {/* 5: as (1) but with binary parameter and response */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+BLING0=", true, "\x00\x01\x02\x04\xff\xfe\xfd\xfc", 8, /* Command with binary parameter */
        NULL, NULL, /* No URC */
        NULL, NULL, 0 /* No prefix or response body */
    },
    {/* 6: as (2) but with binary parameter and response */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+BLING1=", true, "\xff\xfe\xfd\xfc\x03\x02\x01\x00", 8, /* Command with binary parameter */
        NULL, NULL, /* No URC */
        "+BLAH1:", "\x00", 1 /* Information response prefix and body  */
    },
    {/* 7: as (3) but with binary parameter and response */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+BLING2=", true, "\xaa\x55", 2, /* Command with binary parameter */
        "+UBLIM:", "blam", /* URC inserted */
        NULL, NULL, 0 /* No prefix or response body */
    },
    {/* 8: as (4) but with binary parameter and response */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+BLING3=", true, "\x55\xaa", 2, /* Command with binary parameter */
        "+UPIF:", "blammer 1", /* URC inserted */
        "+BLING3:", "\x00\xff\x00\xff", 4 /* Information response prefix and body  */
    },
    {/* 9: as (8) but with V2 scheme */
        true /* V2 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, U_CELL_SEC_C2C_TEST_HMAC_TAG,
        "AT+BLING3=", true, "\x55\xaa", 2, /* Command with binary parameter */
        "+UPIF:", "blammer 2", /* URC inserted */
        "+BLING3:", "\x00\xff\x00\xff", 4 /* Information response prefix and body */
    },
    {   /* 10: as (8) but with command and response of the maximum amount */
        /* of user data that can be fitted into a chunk (which is one less */
        /* than U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES because of the way */
        /* RFC 5652 padding works) */
        /* [This comment done as separate lines and with this exact indentation */
        /* as otherwise AStyle wants to move it another four spaces to the right */
        /* every time it processes it :-)] */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+VERYLONG_V1=", false,  /* Command prefix 15 bytes */
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789"
        "_____0004:01234567890123456789012345678", 239,
        /* (total becomes 255 with \r command delimiter) */
        "+UPUF:", "little URC 1", /* URC inserted */
        "+VERYLONG_V1:", /* Information response prefix 13 bytes */
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789"
        "_____0004:012345678901234567890123456789", 240
        /* (total becomes 255 with \r\n response delimiter) */
    },
    {/* 11: as (10) but with V2 scheme */
        true /* V2 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, U_CELL_SEC_C2C_TEST_HMAC_TAG,
        "AT+VERYLONG_V2=", false,  /* Command prefix 15 bytes */
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789"
        "_____0004:01234567890123456789012345678", 239,
        /* (total becomes 255 with \r command delimiter) */
        "+UPUF:", "little URC 2", /* URC inserted */
        "+VERYLONG_V2:", /* Information response prefix 13 bytes */
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789"
        "_____0004:012345678901234567890123456789", 240
        /* (total becomes 255 with \r\n response delimiter) */
    },
    {/* 12: a real biggee */
        false /* V1 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, NULL,
        "AT+REALLYLONGONE=", false,  /* Command prefix */
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789"
        "_____0004:0123456789012345678901234567890123456789"
        "_____0005:0123456789012345678901234567890123456789"
        "_____0006:0123456789012345678901234567890123456789"
        "_____0007:0123456789012345678901234567890123456789"
        "_____0008:0123456789012345678901234567890123456789"
        "_____0009:0123456789012345678901234567890123456789",
        500,
        "+UPUF:", "little URC 3", /* URC inserted */
        "+ALSOAREALLYLONGONE:", /* Information response prefix */
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789"
        "_____0004:0123456789012345678901234567890123456789"
        "_____0005:0123456789012345678901234567890123456789"
        "_____0006:0123456789012345678901234567890123456789"
        "_____0007:0123456789012345678901234567890123456789"
        "_____0008:0123456789012345678901234567890123456789"
        "_____0009:0123456789012345678901234567890123456789",
        500
    },
    {/* 13: as (12) but with V2 scheme */
        true /* V2 */, U_CELL_SEC_C2C_CHUNK_MAX_LENGTH_BYTES,
        U_CELL_SEC_C2C_TEST_TE_SECRET, U_CELL_SEC_C2C_TEST_KEY, U_CELL_SEC_C2C_TEST_HMAC_TAG,
        "AT+ANOTHERREALLYLONGONE=", false,  /* Command prefix */
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789"
        "_____0004:0123456789012345678901234567890123456789"
        "_____0005:0123456789012345678901234567890123456789"
        "_____0006:0123456789012345678901234567890123456789"
        "_____0007:0123456789012345678901234567890123456789"
        "_____0008:0123456789012345678901234567890123456789"
        "_____0009:0123456789012345678901234567890123456789",
        500,
        "+UPUF:", "little URC 4", /* URC inserted */
        "+ALSOANOTHERREALLYLONGONE:", /* Information response prefix */
        "_____0000:0123456789012345678901234567890123456789"
        "_____0001:0123456789012345678901234567890123456789"
        "_____0002:0123456789012345678901234567890123456789"
        "_____0003:0123456789012345678901234567890123456789"
        "_____0004:0123456789012345678901234567890123456789"
        "_____0005:0123456789012345678901234567890123456789"
        "_____0006:0123456789012345678901234567890123456789"
        "_____0007:0123456789012345678901234567890123456789"
        "_____0008:0123456789012345678901234567890123456789"
        "_____0009:0123456789012345678901234567890123456789",
        500
    }
};

#endif

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Print out text.
static void print(const char *pStr, size_t length)
{
    char c;

    for (size_t x = 0; x < length; x++) {
        c = *pStr++;
        if (!isprint((int32_t) c)) {
            // Print the hex
            uPortLog("[%02x]", c);
        } else {
            // Print the ASCII character
            uPortLog("%c", c);
        }
    }
}

// Print out binary.
//lint -esym(522, printHex) Suppress "lacks side effects", which
// will be true if logging is compiled out
static void printHex(const char *pStr, size_t length)
{
#if U_CFG_ENABLE_LOGGING
    char c;

    for (size_t x = 0; x < length; x++) {
        c = *pStr++;
        uPortLog("[%02x]", c);
    }
#else
    (void) pStr;
    (void) length;
#endif
}

#if (U_CFG_TEST_UART_A >= 0) && (U_CFG_TEST_UART_B >= 0)
// On some platforms printing is line
// buffered so long strings will get lost unless
// they are chunked up: this function
// prints reasonable block sizes
//lint -esym(522, printBlock) Suppress "lacks side effects", which
// will be true if logging is compiled out
static void printBlock(const char *pStr, size_t length,
                       bool isBinary, size_t index)
{
#if U_CFG_ENABLE_LOGGING
    int32_t x = (int32_t) length;
    int32_t y;

    while (x > 0) {
        uPortLog("U_CELL_SEC_C2C_TEST_%d: \"", index);
        y = x;
        if (y > 32) {
            y = 32;
        }
        if (isBinary) {
            printHex(pStr, y);
        } else {
            print(pStr, y);
        }
        uPortLog("\"\n");
        // Don't overwhelm the poor debug output,
        // there there
        uPortTaskBlock(100);
        x -= y;
        pStr += y;
    }
#else
    (void) pStr;
    (void) length;
    (void) index;
    (void) isBinary;
#endif
}
#endif

// Check the result of an encryption.
static void checkEncrypted(size_t testIndex,
                           size_t chunkIndex,
                           const char *pEncrypted,
                           size_t encryptedLength,
                           const uCellSecC2cTest_t *pTestData)
{
    char *pData;
    char *pDecrypted;
    size_t length = 0;
    size_t previousLength = 0;

    // Make sure that testIndex us used to keep
    // compilers happy if logging is compiled out
    (void) testIndex;

    uPortLog("U_CELL_SEC_C2C_TEST_%d: encrypted chunk %d, %d byte(s) \"",
             testIndex + 1, chunkIndex + 1, encryptedLength);
    if (pEncrypted != NULL) {
        printHex(pEncrypted, encryptedLength);
    } else {
        uPortLog("[NULL]");
    }
    uPortLog("\".\n");
    U_PORT_TEST_ASSERT(encryptedLength == pTestData->encryptedLength[chunkIndex]);

    for (size_t x = 0; x < chunkIndex; x++) {
        previousLength += pTestData->clearLength[x];
    }

    if (pEncrypted != NULL) {
        // Decrypt the data block to check if the contents were correct
        memcpy(gBufferB + previousLength, pEncrypted, encryptedLength);
        pData = gBufferB + previousLength;
        length = encryptedLength;
        pDecrypted = pUCellSecC2cInterceptRx(0, &pData, &length,
                                             &gContext);

        uPortLog("U_CELL_SEC_C2C_TEST_%d: decrypted becomes %d byte(s) \"",
                 testIndex + 1, length);
        if (pDecrypted != NULL) {
            print(pDecrypted, length);
        } else {
            uPortLog("[NULL]");
        }
        uPortLog("\".\n");

        U_PORT_TEST_ASSERT(pData == gBufferB + previousLength + encryptedLength);
        U_PORT_TEST_ASSERT(length == pTestData->clearLength[chunkIndex]);
        if (pDecrypted != NULL) {
            U_PORT_TEST_ASSERT(memcmp(pDecrypted, pTestData->pClear + previousLength,
                                      pTestData->clearLength[chunkIndex]) == 0);
        }
    }
}

#if (U_CFG_TEST_UART_A >= 0) && (U_CFG_TEST_UART_B >= 0)

/** Send a thing over a UART.
 */
static int32_t atServerSendThing(int32_t uartHandle,
                                 const char *pThing,
                                 size_t length)
{
    int32_t sizeOrError = 0;

    uPortLog("U_CELL_SEC_C2C_TEST_%d: AT server sending %d byte(s):\n",
             gAtTestCount + 1, length);
    printBlock(pThing, length, true, gAtTestCount + 1);

    while ((length > 0) && (sizeOrError >= 0)) {
        sizeOrError = uPortUartWrite(uartHandle,
                                     pThing, length);
        if (sizeOrError > 0) {
            pThing += sizeOrError;
            length -= sizeOrError;
        }
    }

    return sizeOrError;
}

/** Encrypt and send a buffer of stuff.
 */
static int32_t atServerEncryptAndSendThing(int32_t uartHandle,
                                           const char *pThing,
                                           size_t length,
                                           size_t chunkLengthMax)
{
    int32_t sizeOrError = 0;
    int32_t x;
    const char *pStart = pThing;
    const char *pOut;
    size_t outLength = length;

    // The AT server-side security context will
    // have already been set up, just need to
    // reset a few parameters
    gAtServerContext.pTx->txInLength = 0;
    gAtServerContext.pTx->txInLimit = chunkLengthMax;

    while ((pThing < pStart + length) && (sizeOrError >= 0)) {
        pOut = pUCellSecC2cInterceptTx(0, &pThing, &outLength,
                                       &gAtServerContext);
        if (outLength > 0) {
            // More than a chunk's worth must have accumulated,
            // send it
            x = atServerSendThing(uartHandle, pOut, outLength);
            if (x >= 0) {
                sizeOrError += x;
            } else {
                sizeOrError = x;
            }
        }
        outLength = length - (pThing - pStart);
    }

    if (sizeOrError >= 0) {
        // Flush the remainder out of the encryption function
        // by calling it again with NULL
        outLength = 0;
        pOut = pUCellSecC2cInterceptTx(0, NULL, &outLength,
                                       &gAtServerContext);
        if (outLength > 0) {
            x = atServerSendThing(uartHandle, pOut, outLength);
            if (x >= 0) {
                sizeOrError += x;
            } else {
                sizeOrError = x;
            }
        }
    }

    return sizeOrError;
}

// Callback which receives commands, decrypts them, checks them
// and then sends back potentially a URC and a response.
//lint -e{818} suppress "could be declared as pointing to const", callback
// has to follow function signature
static void atServerCallback(int32_t uartHandle, uint32_t eventBitmask,
                             void *pParameters)
{
    size_t x;
    size_t y;
    size_t z;
    int32_t sizeOrError = -1;
    const uCellSecC2cTestAt_t *pTestAt = *((uCellSecC2cTestAt_t **) pParameters);
    size_t length = 0;
    size_t interceptLength = 0;
    char *pData;
    char *pDecrypted;
#if U_CFG_OS_CLIB_LEAKS
    int32_t heapUsed;
#endif

    if ((pTestAt != NULL) &&
        (eventBitmask & U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED)) {
        sizeOrError = 0;
        // Loop until no characters left to receive
        while ((uPortUartGetReceiveSize(uartHandle) > 0) && (sizeOrError >= 0)) {
            sizeOrError = uPortUartRead(uartHandle, gBufferA + length,
                                        sizeof(gBufferA) - length);
            if (sizeOrError > 0) {
                length += sizeOrError;
                if (length >= sizeof(gBufferA)) {
                    length = 0;
                    sizeOrError = -1;
                }
            }
            // Wait long enough for everything to have been received
            // and for any prints in the sending task to be printed
            uPortTaskBlock(1000);
        }

        if (sizeOrError > 0) {
#if U_CFG_OS_CLIB_LEAKS
            // Calling printf() from a new task causes newlib
            // to allocate additional memory which, depending
            // on the OS/system, may not be recovered;
            // take account of that here.
            heapUsed = uPortGetHeapFree();
#endif
            uPortLog("U_CELL_SEC_C2C_TEST_%d: AT server received, %d byte(s):\n",
                     gAtTestCount + 1, length);
            printBlock(gBufferA, length, true, gAtTestCount + 1);

#if U_CFG_OS_CLIB_LEAKS
            // Take account of any heap lost through the first
            // printf()
            gSystemHeapLost += (size_t) (unsigned) (heapUsed - uPortGetHeapFree());
#endif

            // Decrypt the received chunk or chunks in place
            // by calling pUCellSecC2cInterceptRx with
            // the server context.
            pData = gBufferA;
            x = length;
            interceptLength = length;
            sizeOrError = 0;
            while ((x > 0) && (sizeOrError >= 0)) {
                pDecrypted = pUCellSecC2cInterceptRx(0, &pData,
                                                     &interceptLength,
                                                     &gAtServerContext);
                if (pDecrypted != NULL) {
                    // Our intercept function always returns a pointer
                    // to the start of the buffer, to pData, so just need
                    // to shuffle everything down so that the next pData
                    // we provide to the intercept function will be
                    // contiguous with the already decrypted data.
                    // The buffer is as below where "sizeOrError"
                    // is the previously decrypted data, "interceptLength"
                    // the newly decrypted data and "pData" is where
                    // we've got to in the buffer.
                    //
                    //                       |-------------------- X ------------------|
                    //    +------------------+-----------------+-----------------------+
                    //    |    sizeOrError   | interceptLength |                       |
                    //    +------------------+-----------------+-------+---------------+
                    // gBufferA         pDecrypted                   pData
                    //                                                 |------ Y ------|
                    //                                         |-- Z --|
                    // y is the amount of data to move
                    y = gBufferA + sizeOrError + x - pData;
                    // Grow size
                    sizeOrError += (int32_t) interceptLength;
                    // Do the move
                    memmove(gBufferA + sizeOrError, pData, y);
                    // z is the distance it was moved
                    z = pData - (gBufferA + sizeOrError);
                    // Shift pData down to match
                    pData -= z;
                    // Reduce the amount of data left to process
                    x -= z + interceptLength;
                    // The length passed to the intercept function
                    // becomes what we moved
                    interceptLength = y;
                } else {
                    uPortLog("U_CELL_SEC_C2C_TEST_%d: AT server could only"
                             " decrypt %d byte(s).\n", sizeOrError);
                    sizeOrError = -500;
                }
            }

            if (sizeOrError > 0) {
                uPortLog("U_CELL_SEC_C2C_TEST_%d: AT server decrypted %d byte(s):\n",
                         gAtTestCount + 1, sizeOrError);
                printBlock(gBufferA, sizeOrError, false, gAtTestCount + 1);

                x = strlen(pTestAt->pCommandPrefix);
                if (sizeOrError == x + pTestAt->commandBodyLength +
                    U_AT_CLIENT_COMMAND_DELIMITER_LENGTH_BYTES) {
                    if (memcmp(gBufferA, pTestAt->pCommandPrefix, x) == 0) {
                        if (memcmp(gBufferA + x, pTestAt->pCommandBody,
                                   pTestAt->commandBodyLength) == 0) {
                            // Should be the correct command delimiter on the end
                            if (memcmp(gBufferA + x + pTestAt->commandBodyLength,
                                       U_AT_CLIENT_COMMAND_DELIMITER,
                                       U_AT_CLIENT_COMMAND_DELIMITER_LENGTH_BYTES) != 0) {
                                uPortLog("U_CELL_SEC_C2C_TEST_%d: expected command"
                                         " delimiter \"");
                                printHex(U_AT_CLIENT_COMMAND_DELIMITER,
                                         U_AT_CLIENT_COMMAND_DELIMITER_LENGTH_BYTES);
                                uPortLog("\" but received \"");
                                printHex(gBufferA + x + pTestAt->commandBodyLength,
                                         U_AT_CLIENT_COMMAND_DELIMITER_LENGTH_BYTES);
                                uPortLog("\".\n");
                                sizeOrError = -400;
                            }
                        } else {
                            uPortLog("U_CELL_SEC_C2C_TEST_%d: expected"
                                     " command body \"", gAtTestCount + 1);
                            if (pTestAt->isBinary) {
                                printHex(pTestAt->pCommandBody, pTestAt->commandBodyLength);
                            } else {
                                print(pTestAt->pCommandBody, pTestAt->commandBodyLength);
                            }
                            uPortLog("\"\n but received \"");
                            if (pTestAt->isBinary) {
                                printHex(gBufferA + x, sizeOrError - x);
                            } else {
                                print(gBufferA + x, sizeOrError - x);
                            }
                            uPortLog("\".\n");
                            sizeOrError = -300;
                        }
                    } else {
                        uPortLog("U_CELL_SEC_C2C_TEST_%d: expected"
                                 " command prefix \"", gAtTestCount + 1);
                        print(pTestAt->pCommandPrefix, x);
                        uPortLog("\"\n but received \"");
                        print(gBufferA, x);
                        uPortLog("\".\n");
                        sizeOrError = -200;
                    }
                } else {
                    uPortLog("U_CELL_SEC_C2C_TEST_%d: expected"
                             " command to be of total length %d"
                             " (including terminator) but was %d.\n",
                             gAtTestCount + 1, x + pTestAt->commandBodyLength + 1,
                             length);
                    sizeOrError = -100;
                }

                // If there is one, assemble and encrypt a URC
                if ((pTestAt->pUrcPrefix != NULL) && (sizeOrError >= 0)) {
                    uPortLog("U_CELL_SEC_C2C_TEST_%d: AT server inserting"
                             " URC \"%s %s\".\n", gAtTestCount + 1,
                             pTestAt->pUrcPrefix, pTestAt->pUrcBody);
                    strcpy(gBufferA, pTestAt->pUrcPrefix);
                    strcat(gBufferA, pTestAt->pUrcBody);
                    strcat(gBufferA, "\r\n");
                    sizeOrError = atServerEncryptAndSendThing(uartHandle,
                                                              gBufferA,
                                                              strlen(gBufferA),
                                                              pTestAt->chunkLengthMax);
                }

                if (sizeOrError >= 0) {
                    // Assemble and encrypt the response
                    uPortLog("U_CELL_SEC_C2C_TEST_%d: AT server sending response:\n",
                             gAtTestCount + 1);
                    if ((pTestAt->pResponsePrefix != NULL) || (pTestAt->pResponseBody != NULL)) {
                        if (pTestAt->pResponsePrefix != NULL) {
                            uPortLog("U_CELL_SEC_C2C_TEST_%d: \"%s\" ...and then:\n",
                                     gAtTestCount + 1, pTestAt->pResponsePrefix);
                        }
                        if (pTestAt->pResponseBody != NULL) {
                            printBlock(pTestAt->pResponseBody,
                                       pTestAt->responseBodyLength,
                                       false, gAtTestCount + 1);
                        } else {
                            uPortLog("U_CELL_SEC_C2C_TEST_%d: [nothing]\n",
                                     gAtTestCount + 1);
                        }
                    } else {
                        uPortLog("U_CELL_SEC_C2C_TEST_%d: [nothing]\n", gAtTestCount + 1);
                    }
                    uPortLog("U_CELL_SEC_C2C_TEST_%d: ...and then \"OK\".\n", gAtTestCount + 1);
                    gBufferA[0] = 0;
                    if (pTestAt->pResponsePrefix != NULL) {
                        strcpy(gBufferA, pTestAt->pResponsePrefix);
                    }
                    x = strlen(gBufferA);
                    if (pTestAt->pResponseBody != NULL) {
                        memcpy(gBufferA + x, pTestAt->pResponseBody,
                               pTestAt->responseBodyLength);
                        x += pTestAt->responseBodyLength;
                    }
                    memcpy(gBufferA + x, "\r\nOK\r\n", 6);
                    x += 6;
                    sizeOrError = atServerEncryptAndSendThing(uartHandle,
                                                              gBufferA, x,
                                                              pTestAt->chunkLengthMax);
                }
            }
        }
    }

    gAtServerErrorOrSize = sizeOrError;
}

// The URC handler for these tests.
//lint -e{818} suppress "could be declared as pointing to const", callback
// has to follow function signature
static void urcHandler(uAtClientHandle_t atClientHandle, void *pParameters)
{
    size_t x = 0;
    int32_t sizeOrError;
    const uCellSecC2cTestAt_t *pTestAt = (uCellSecC2cTestAt_t *) pParameters;
#if U_CFG_OS_CLIB_LEAKS
    int32_t heapUsed;
#endif

    // Read the single string parameter
    sizeOrError = uAtClientReadString(atClientHandle, gBufferC,
                                      sizeof(gBufferC), false);
    if (pTestAt != NULL) {
        if (pTestAt->pUrcBody != NULL) {
            x = strlen(pTestAt->pUrcBody);
        }
#if U_CFG_OS_CLIB_LEAKS
        // Calling printf() from a new task causes newlib
        // to allocate additional memory which, depending
        // on the OS/system, may not be recovered;
        // take account of that here.
        heapUsed = uPortGetHeapFree();
#endif
        uPortLog("U_CELL_SEC_C2C_TEST_%d: AT client received URC \"%s ",
                 gAtTestCount + 1, pTestAt->pUrcPrefix);
        print(gBufferC, x);
        uPortLog("\".\n");
#if U_CFG_OS_CLIB_LEAKS
        // Take account of any heap lost through the first
        // printf()
        gSystemHeapLost += (size_t) (unsigned) (heapUsed - uPortGetHeapFree());
#endif
        if (sizeOrError == x) {
            if (memcmp(gBufferC, pTestAt->pUrcBody, x) != 0) {
                uPortLog("U_CELL_SEC_C2C_TEST_%d: AT client expected"
                         " URC body \"", gAtTestCount + 1);
                print(pTestAt->pUrcBody, x);
                uPortLog("\".\n");
                sizeOrError = -800;
            }
        } else {
            uPortLog("U_CELL_SEC_C2C_TEST_%d: AT client expected"
                     " URC body to be of length %d"
                     "  but was %d.\n",
                     gAtTestCount + 1, x, sizeOrError);
            sizeOrError = -700;
        }
    } else {
#if U_CFG_OS_CLIB_LEAKS
        // Calling printf() from a new task causes newlib
        // to allocate additional memory which, depending
        // on the OS/system, may not be recovered;
        // take account of that here.
        heapUsed = uPortGetHeapFree();
#endif
        uPortLog("U_CELL_SEC_C2C_TEST_%d: AT client received"
                 " URC fragment \"", gAtTestCount + 1);
        print(gBufferC, sizeOrError);
        uPortLog("\" when there wasn't meant to be one.\n");
#if U_CFG_OS_CLIB_LEAKS
        // Take account of any heap lost through the first
        // printf()
        gSystemHeapLost += (size_t) (unsigned) (heapUsed - uPortGetHeapFree());
#endif
        sizeOrError = -600;
    }

    gUrcCount++;
    gUrcErrorOrSize = sizeOrError;
}

#endif

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

/** Test the transmit and receive intercept functions standalone.
 */
U_PORT_TEST_FUNCTION("[cellSecC2c]", "cellSecC2cIntercept")
{
    uCellSecC2cTest_t *pTestData;
    const char *pData;
    const char *pDataStart;
    const char *pOut;
    size_t totalLength;
    size_t outLength;
    size_t numChunks;
    int32_t heapUsed;

    // Whatever called us likely initialised the
    // port so deinitialise it here to obtain the
    // correct initial heap size
    uPortDeinit();

    // On some platforms (e.g. ESP32) the crypto libraries,
    // which the underlying chip-to-chip encryption functions
    // call, allocate a semaphore when they are first called
    // which is never deleted.  To avoid that getting in their
    // way of our heap loss calculation, make a call to one
    // of the crypto functions here.
    uPortCryptoSha256(NULL, 0, gBufferA);

    heapUsed = uPortGetHeapFree();
    U_PORT_TEST_ASSERT(uPortInit() == 0);

    uPortLog("U_CELL_SEC_C2C_TEST: testing chip-to-chip encryption"
             " and decryption intercept functions standalone.\n");

    gContext.pTx = &gContextTx;
    gContext.pRx = &gContextRx;

    for (size_t x = 0; x < sizeof(gTestData) / sizeof(gTestData[0]); x++) {
        pTestData = &gTestData[x];
        totalLength = 0;
        for (size_t y = 0; y < (sizeof(pTestData->clearLength) /
                                sizeof(pTestData->clearLength[0])); y++) {
            totalLength += pTestData->clearLength[y];
        }
        uPortLog("U_CELL_SEC_C2C_TEST_%d: clear text %d byte(s) \"",
                 x + 1, totalLength);
        print(pTestData->pClear, totalLength);
        uPortLog("\".\n");

        // Populate context
        gContext.isV2 = pTestData->isV2;
        memcpy(gContext.teSecret, pTestData->pTeSecret,
               sizeof(gContext.teSecret));
        memcpy(gContext.key, pTestData->pKey,
               sizeof(gContext.key));
        if (pTestData->pHmacTag != NULL) {
            memcpy(gContext.hmacKey, pTestData->pHmacTag,
                   sizeof(gContext.hmacKey));
        }
        gContext.pTx->txInLength = 0;
        gContext.pTx->txInLimit = pTestData->chunkLengthMax;

        memcpy(gBufferA, pTestData->pClear, totalLength);
        pData = gBufferA;
        numChunks = 0;
        pDataStart = pData;

        // Do the encryption by calling the transmit intercept
        do {
            U_PORT_TEST_ASSERT(numChunks < pTestData->numChunks);
            outLength = totalLength - (pData - pDataStart);
            pOut = pUCellSecC2cInterceptTx(0, &pData, &outLength,
                                           &gContext);
            if (outLength > 0) {
                // There will only be a result here if the input reached
                // the chunk length limit
                checkEncrypted(x, numChunks, pOut, outLength, pTestData);
                numChunks++;
            }
        } while (pData < gBufferA + totalLength);

        // Flush the transmit intercept by calling it again with NULL
        outLength = 0;
        pOut = pUCellSecC2cInterceptTx(0, NULL, &outLength, &gContext);
        if (outLength > 0) {
            checkEncrypted(x, numChunks, pOut, outLength, pTestData);
            numChunks++;
        }

        U_PORT_TEST_ASSERT(numChunks == pTestData->numChunks);
        // When done, the RX buffer should contain the complete
        // clear message
        U_PORT_TEST_ASSERT(memcmp(gBufferB, pTestData->pClear,
                                  totalLength) == 0);
    }

    uPortDeinit();

#ifndef __XTENSA__
    // Check for memory leaks
    // TODO: this if'ed out for ESP32 (xtensa compiler) at
    // the moment as there is an issue with ESP32 hanging
    // on to memory in the UART drivers that can't easily be
    // accounted for.
    heapUsed -= uPortGetHeapFree();
    uPortLog("U_CELL_SEC_C2C_TEST: we have leaked %d byte(s).\n",
             heapUsed);
    // heapUsed < 0 for the Zephyr case where the heap can look
    // like it increases (negative leak)
    U_PORT_TEST_ASSERT(heapUsed <= 0);
#else
    (void) heapUsed;
#endif
}

#if (U_CFG_TEST_UART_A >= 0) && (U_CFG_TEST_UART_B >= 0)

/** Test use of the intercept functions inside the AT client
 * with a dummy AT server to loop stuff back to us.
 * NOTE: this test is a bit of a balancing act; need to
 * print lots of debug so that we can see what's going on
 * in case there's a problem but at the same time it has two
 * independent tasks running between two actual serial ports
 * without flow control (out of pins) and with deliberate
 * timing constraints in the AT client.  So, it works, but
 * I suggest you don't fiddle with any of the timings, it's
 * quite carefully tuned to work on all platforms.
 */
U_PORT_TEST_FUNCTION("[cellSecC2c]", "cellSecC2cAtClient")
{
    uAtClientHandle_t atClientHandle;
    int32_t sizeOrError;
    const uCellSecC2cTestAt_t *pTestAt = NULL;
    const char *pLastAtPrefix = NULL;
    size_t urcCount = 0;
    int32_t stackMinFreeBytes;
    int32_t heapUsed;
    int32_t heapClibLossOffset = (int32_t) gSystemHeapLost;

    gContext.pTx = &gContextTx;
    gContext.pRx = &gContextRx;

    // Whatever called us likely initialised the
    // port so deinitialise it here to obtain the
    // correct initial heap size
    uPortDeinit();

    // On some platforms (e.g. ESP32) the crypto libraries,
    // which the underlying chip-to-chip encryption functions
    // call, allocate a semaphore when they are first called
    // which is never deleted.  To avoid that getting in their
    // way of our heap loss calculation, make a call to one
    // of the crypto functions here.
    uPortCryptoSha256(NULL, 0, gBufferA);

    heapUsed = uPortGetHeapFree();

    uPortLog("U_CELL_SEC_C2C_TEST: testing chip-to-chip encryption"
             " and decryption intercept functions inside an AT client.\n");

    U_PORT_TEST_ASSERT(uPortInit() == 0);

    gUartAHandle = uPortUartOpen(U_CFG_TEST_UART_A,
                                 U_CFG_TEST_BAUD_RATE,
                                 NULL,
                                 U_CFG_TEST_UART_BUFFER_LENGTH_BYTES,
                                 U_CFG_TEST_PIN_UART_A_TXD,
                                 U_CFG_TEST_PIN_UART_A_RXD,
                                 U_CFG_TEST_PIN_UART_A_CTS,
                                 U_CFG_TEST_PIN_UART_A_RTS);
    U_PORT_TEST_ASSERT(gUartAHandle >= 0);

    uPortLog("U_CELL_SEC_C2C_TEST: AT client will be on UART %d,"
             " TXD pin %d (0x%02x) and RXD pin %d (0x%02x).\n",
             U_CFG_TEST_UART_A, U_CFG_TEST_PIN_UART_A_TXD,
             U_CFG_TEST_PIN_UART_A_TXD, U_CFG_TEST_PIN_UART_A_RXD,
             U_CFG_TEST_PIN_UART_A_RXD);

    gUartBHandle = uPortUartOpen(U_CFG_TEST_UART_B,
                                 U_CFG_TEST_BAUD_RATE,
                                 NULL,
                                 U_CFG_TEST_UART_BUFFER_LENGTH_BYTES,
                                 U_CFG_TEST_PIN_UART_B_TXD,
                                 U_CFG_TEST_PIN_UART_B_RXD,
                                 U_CFG_TEST_PIN_UART_B_CTS,
                                 U_CFG_TEST_PIN_UART_B_RTS);
    U_PORT_TEST_ASSERT(gUartBHandle >= 0);

    uPortLog("U_CELL_SEC_C2C_TEST: AT server will be on UART %d,"
             " TXD pin %d (0x%02x) and RXD pin %d (0x%02x).\n",
             U_CFG_TEST_UART_B, U_CFG_TEST_PIN_UART_B_TXD,
             U_CFG_TEST_PIN_UART_B_TXD, U_CFG_TEST_PIN_UART_B_RXD,
             U_CFG_TEST_PIN_UART_B_RXD);

    uPortLog("U_CELL_SEC_C2C_TEST: make sure these pins are"
             " cross-connected.\n");

    // Set up an AT server event handler on UART 1, running
    // at URC priority for convenience
    // This event handler receives our encrypted chunks, decrypts
    // them and sends back an encrypted response for us to decrypt.
    U_PORT_TEST_ASSERT(uPortUartEventCallbackSet(gUartBHandle,
                                                 U_PORT_UART_EVENT_BITMASK_DATA_RECEIVED,
                                                 atServerCallback, (void *) &pTestAt,
                                                 U_AT_CLIENT_URC_TASK_STACK_SIZE_BYTES,
                                                 U_AT_CLIENT_URC_TASK_PRIORITY) == 0);

    U_PORT_TEST_ASSERT(uAtClientInit() == 0);

    uPortLog("U_CELL_SEC_C2C_TEST: adding an AT client on UART %d...\n",
             U_CFG_TEST_UART_A);
    atClientHandle = uAtClientAdd(gUartAHandle, U_AT_CLIENT_STREAM_TYPE_UART,
                                  NULL, U_CELL_AT_BUFFER_LENGTH_BYTES);
    U_PORT_TEST_ASSERT(atClientHandle != NULL);

    // Add transmit and receive intercepts
    uAtClientStreamInterceptTx(atClientHandle, pUCellSecC2cInterceptTx,
                               (void *) &gContext);
    uAtClientStreamInterceptRx(atClientHandle, pUCellSecC2cInterceptRx,
                               (void *) &gContext);

    uPortLog("U_CELL_SEC_C2C_TEST: %d chunks(s) to execute.\n",
             sizeof(gTestAt) / sizeof(gTestAt[0]));
    for (size_t x = 0; x < sizeof(gTestAt) / sizeof(gTestAt[0]); x++) {
        pTestAt = &(gTestAt[x]);

        // Populate the AT client-side chip to chip
        // security context
        gContext.isV2 = pTestAt->isV2;
        memcpy(gContext.teSecret, pTestAt->pTeSecret,
               sizeof(gContext.teSecret));
        memcpy(gContext.key, pTestAt->pKey,
               sizeof(gContext.key));
        if (pTestAt->pHmacTag != NULL) {
            memcpy(gContext.hmacKey, pTestAt->pHmacTag,
                   sizeof(gContext.hmacKey));
        }
        gContext.pTx->txInLimit = pTestAt->chunkLengthMax;

        // Copy this into the AT server-side chip to chip
        // security context
        memcpy(&gAtServerContext, &gContext,
               sizeof(gAtServerContext));
        gAtServerContext.pRx = &gAtServerContextRx;
        gAtServerContext.pTx = &gAtServerContextTx;

        if (pTestAt->pUrcPrefix != NULL) {
            urcCount++;
        }
        // Add a URC handler if there is one, removing the old one
        if (pTestAt->pUrcPrefix != NULL) {
            if (pLastAtPrefix != NULL) {
                uAtClientRemoveUrcHandler(atClientHandle, pLastAtPrefix);
            }
            // GCC can complain here that
            // we're passing a const pointer
            // (pTestAt) as a parameter
            // that is not const.
            // Since this is an anonymous parameter
            // passed to a callback we have no
            // choice, the callback itself
            // has to know how to behave, we
            // can't dictate that.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
            U_PORT_TEST_ASSERT(uAtClientSetUrcHandler(atClientHandle,
                                                      pTestAt->pUrcPrefix,
                                                      urcHandler,
                                                      //lint -e(605) Suppress "increase in pointer capability":
                                                      // pTestAt definitely points to a const but the URC handler
                                                      // parameter is a generic part of a callback which can't know
                                                      // that.
                                                      (const void *) pTestAt) == 0);
#pragma GCC diagnostic pop
            pLastAtPrefix = pTestAt->pUrcPrefix;
        }

        // Send the AT string: we only test sending strings or
        // binary here, the other uAtClientWritexxx
        // operations are assumed to work in the same way
        uPortLog("U_CELL_SEC_C2C_TEST_%d: AT client sending: \"%s\" and then...\n",
                 x + 1, pTestAt->pCommandPrefix);
        printBlock(pTestAt->pCommandBody,
                   pTestAt->commandBodyLength,
                   pTestAt->isBinary, x + 1);

        uAtClientLock(atClientHandle);

        // We do a LOT of debug prints in the AT server task which responds
        // to this and we have to take our time with them so as not to
        // overload the debug stream on some platforms so give it plenty
        // of time to respond.
        uAtClientTimeoutSet(atClientHandle, 20000);
        uAtClientCommandStart(atClientHandle, pTestAt->pCommandPrefix);
        if (pTestAt->isBinary) {
            // Binary bytes
            uAtClientWriteBytes(atClientHandle, pTestAt->pCommandBody,
                                pTestAt->commandBodyLength, false);
        } else {
            // String without quotes
            uAtClientWriteString(atClientHandle, pTestAt->pCommandBody, false);
        }
        uAtClientCommandStop(atClientHandle);

        uPortLog("U_CELL_SEC_C2C_TEST_%d: AT client waiting for response",
                 x + 1);
        if (pTestAt->pResponsePrefix != NULL) {
            uPortLog(" \"%s\"", pTestAt->pResponsePrefix);
        }
        uPortLog("...\n");

        uAtClientResponseStart(atClientHandle, pTestAt->pResponsePrefix);
        if (pTestAt->isBinary) {
            // Standalone bytes
            sizeOrError = uAtClientReadBytes(atClientHandle, gBufferB,
                                             sizeof(gBufferB), true);
        } else {
            // Quoted string
            sizeOrError = uAtClientReadString(atClientHandle, gBufferB,
                                              sizeof(gBufferB), false);
        }
        uAtClientResponseStop(atClientHandle);

        // Wait a moment before printing so that any URCs get to
        // be printed without us trampling over them
        uPortTaskBlock(1000);
        uPortLog("U_CELL_SEC_C2C_TEST_%d: AT client read result is %d.\n",
                 x + 1, sizeOrError);
        U_PORT_TEST_ASSERT(sizeOrError >= 0);
        uPortLog("U_CELL_SEC_C2C_TEST_%d: AT client received response:\n", x + 1);
        if (sizeOrError > 0) {
            if (pTestAt->pResponsePrefix != NULL) {
                uPortLog("U_CELL_SEC_C2C_TEST_%d: \"%s\" and then...\n",
                         x + 1, pTestAt->pResponsePrefix);
            }
            printBlock(gBufferB, sizeOrError,
                       pTestAt->isBinary, x + 1);
        } else {
            uPortLog("U_CELL_SEC_C2C_TEST_%d:  [nothing]\n", x + 1);
        }

        U_PORT_TEST_ASSERT(uAtClientUnlock(atClientHandle) == 0);

        U_PORT_TEST_ASSERT(sizeOrError == pTestAt->responseBodyLength);
        if (sizeOrError > 0) {
            U_PORT_TEST_ASSERT(memcmp(gBufferB, pTestAt->pResponseBody,
                                      pTestAt->responseBodyLength) == 0);
        }

        U_PORT_TEST_ASSERT(gAtServerErrorOrSize >= 0);
        U_PORT_TEST_ASSERT(gUrcErrorOrSize >= 0);
        U_PORT_TEST_ASSERT(urcCount == gUrcCount);
        uPortLog("U_CELL_SEC_C2C_TEST_%d: ...and then \"OK\"\n", x + 1);
        gAtTestCount++;
        // Wait between iterations to avoid the debug
        // streams overunning
        uPortTaskBlock(1000);
    }
    U_PORT_TEST_ASSERT(gAtTestCount == sizeof(gTestAt) / sizeof(gTestAt[0]));

    stackMinFreeBytes = uAtClientUrcHandlerStackMinFree(atClientHandle);
    uPortLog("U_CELL_SEC_C2C_TEST: AT client URC task had min %d byte(s)"
             " stack free out of %d.\n", stackMinFreeBytes,
             U_AT_CLIENT_URC_TASK_STACK_SIZE_BYTES);
    U_PORT_TEST_ASSERT(stackMinFreeBytes > 0);

    stackMinFreeBytes = uAtClientCallbackStackMinFree();
    uPortLog("U_CELL_SEC_C2C_TEST: AT client callback task had min"
             " %d byte(s) stack free out of %d.\n", stackMinFreeBytes,
             U_AT_CLIENT_CALLBACK_TASK_STACK_SIZE_BYTES);
    U_PORT_TEST_ASSERT(stackMinFreeBytes > 0);

    // Check the stack extent for the task on the end of the
    // event queue
    stackMinFreeBytes = uPortUartEventStackMinFree(gUartBHandle);
    uPortLog("U_CELL_SEC_C2C_TEST: the AT server event queue task had"
             " %d byte(s) free out of %d.\n",
             stackMinFreeBytes, U_AT_CLIENT_URC_TASK_STACK_SIZE_BYTES);
    U_PORT_TEST_ASSERT(stackMinFreeBytes > 0);

    uPortLog("U_CELL_SEC_C2C_TEST: removing AT client...\n");
    uAtClientRemove(atClientHandle);
    uAtClientDeinit();

    uPortUartClose(gUartBHandle);
    gUartBHandle = -1;
    uPortUartClose(gUartAHandle);
    gUartAHandle = -1;
    uPortDeinit();

    // Check for memory leaks
    heapUsed -= uPortGetHeapFree();
    uPortLog("U_CELL_SEC_C2C_TEST: %d byte(s) of heap were lost to"
             " the C library during this test and we have"
             " leaked %d byte(s).\n",
             gSystemHeapLost - heapClibLossOffset,
             heapUsed - (gSystemHeapLost - heapClibLossOffset));
    // heapUsed < 0 for the Zephyr case where the heap can look
    // like it increases (negative leak)
    U_PORT_TEST_ASSERT((heapUsed < 0) ||
                       (heapUsed <= ((int32_t) gSystemHeapLost) - heapClibLossOffset));
}

#endif

/** Clean-up to be run at the end of this round of tests, just
 * in case there were test failures which would have resulted
 * in the deinitialisation being skipped.
 */
U_PORT_TEST_FUNCTION("[cellSecC2c]", "cellSecC2cCleanUp")
{
    int32_t minFreeStackBytes;

    uAtClientDeinit();
    if (gUartAHandle >= 0) {
        uPortUartClose(gUartAHandle);
    }
    if (gUartBHandle >= 0) {
        uPortUartClose(gUartBHandle);
    }

    minFreeStackBytes = uPortTaskStackMinFree(NULL);
    uPortLog("U_CELL_SEC_C2C_TEST: main task stack had a minimum of"
             " %d byte(s) free at the end of these tests.\n",
             minFreeStackBytes);
    U_PORT_TEST_ASSERT(minFreeStackBytes >=
                       U_CFG_TEST_OS_MAIN_TASK_MIN_FREE_STACK_BYTES);

    uPortDeinit();
}

// End of file
