//-----------------------------------------------------------------------------
// Copyright (C) 2014 Iceman
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// High frequency MIFARE Desfire commands
//-----------------------------------------------------------------------------
#include "cmdhfmfdes.h"

#include <stdio.h>
#include <string.h>

#include "commonutil.h"  // ARRAYLEN
#include "cmdparser.h"    // command_t
#include "comms.h"
#include "ui.h"
#include "cmdhw.h"
#include "cmdhf14a.h"
#include "mbedtls/des.h"
#include "crypto/libpcrypto.h"
#include "protocols.h"
#include "mifare.h"         // desfire raw command options
#include "cmdtrace.h"
#include "cliparser/cliparser.h"
#include "emv/apduinfo.h"   // APDU manipulation / errorcodes
#include "emv/emvcore.h"    // APDU logging
#include "util_posix.h"     // msleep
#include "mifare/mifare4.h" // MIFARE Authenticate / MAC

uint8_t key_zero_data[16] = { 0x00 };
uint8_t key_ones_data[16] = { 0x01 };
uint8_t key_defa_data[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
uint8_t key_picc_data[16] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f };

typedef enum {
    UNKNOWN = 0,
    MF3ICD40,
    EV1,
    EV2,
    EV3,
    LIGHT,
} desfire_cardtype_t;

typedef struct {
    uint8_t aid[3];
    uint8_t fid[2];
    uint8_t name[16];
} dfname_t;

static int CmdHelp(const char *Cmd);

/*
         uint8_t cmd[3 + 16] = {0xa8, 0x90, 0x90, 0x00};
                int res = ExchangeRAW14a(cmd, sizeof(cmd), false, false, data, sizeof(data), &datalen, false);

                if (!res && datalen > 1 && data[0] == 0x09) {
                    SLmode = 0;
                }

*/

int DESFIRESendApdu(bool activate_field, bool leavefield_on, sAPDU apdu, uint8_t *result, int max_result_len, int *result_len, uint16_t *sw) {

    *result_len = 0;
    if (sw) *sw = 0;

    uint16_t isw = 0;
    int res = 0;

    if (activate_field) {
        DropField();
        msleep(50);
    }

    // select?
    uint8_t data[APDU_RES_LEN] = {0};

    // COMPUTE APDU
    int datalen = 0;
    //if (APDUEncodeS(&apdu, false, IncludeLe ? 0x100 : 0x00, data, &datalen)) {
    if (APDUEncodeS(&apdu, false, 0x100, data, &datalen)) {
        PrintAndLogEx(ERR, "APDU encoding error.");
        return PM3_EAPDU_ENCODEFAIL;
    }

    if (GetAPDULogging() || (g_debugMode > 1))
        PrintAndLogEx(SUCCESS, ">>>> %s", sprint_hex(data, datalen));

    res = ExchangeAPDU14a(data, datalen, activate_field, leavefield_on, result, max_result_len, result_len);
    if (res) {
        return res;
    }

    if (GetAPDULogging() || (g_debugMode > 1))
        PrintAndLogEx(SUCCESS, "<<<< %s", sprint_hex(result, *result_len));

    if (*result_len < 2) {
        return PM3_SUCCESS;
    }

    *result_len -= 2;
    isw = (result[*result_len] << 8) + result[*result_len + 1];
    if (sw)
        *sw = isw;

    if (isw != 0x9000 && isw != MFDES_SUCCESS_FRAME_RESP && isw != MFDES_ADDITIONAL_FRAME_RESP) {
        if (GetAPDULogging()) {
            if (isw >> 8 == 0x61) {
                PrintAndLogEx(ERR, "APDU chaining len:%02x -->", isw & 0xff);
            } else {
                PrintAndLogEx(ERR, "APDU(%02x%02x) ERROR: [%4X] %s", apdu.CLA, apdu.INS, isw, GetAPDUCodeDescription(isw >> 8, isw & 0xff));
                return PM3_EAPDU_FAIL;
            }
        }
    }

    return PM3_SUCCESS;
}


static int send_desfire_cmd(sAPDU *apdu, bool select, uint8_t *dest, int *recv_len, uint16_t *sw, int splitbysize) {
    //SetAPDULogging(true);
    *sw = 0;
    uint8_t data[255 * 5]  = {0x00};
    int resplen = 0;
    int pos = 0;
    int i = 1;
    int res = DESFIRESendApdu(select, true, *apdu, data, sizeof(data), &resplen, sw);
    if (res != PM3_SUCCESS) return res;
    if (*sw != MFDES_ADDITIONAL_FRAME_RESP && *sw != MFDES_SUCCESS_FRAME_RESP) return PM3_ESOFT;
    if (dest != NULL) {
        memcpy(dest, data, resplen);
    }

    pos += resplen;
    if (*sw == MFDES_ADDITIONAL_FRAME_RESP) {
        apdu->INS = MFDES_ADDITIONAL_FRAME; //0xAF

        res = DESFIRESendApdu(false, true, *apdu, data, sizeof(data), &resplen, sw);
        if (res != PM3_SUCCESS) return res;
        if (dest != NULL) {
            if (splitbysize) {
                memcpy(&dest[i * splitbysize], data, resplen);
                i += 1;
            } else {
                memcpy(&dest[pos], data, resplen);
            }
        }
        pos += resplen;
    }
    if (splitbysize) *recv_len = i;
    else {
        *recv_len = pos;
    }
    //SetAPDULogging(false);
    return PM3_SUCCESS;

}

static desfire_cardtype_t getCardType(uint8_t major, uint8_t minor) {

    if (major == 0x00)
        return MF3ICD40;
    else if (major == 0x01 && minor == 0x00)
        return EV1;
    else if (major == 0x12 && minor == 0x00)
        return EV2;
//    else if (major == 0x13 && minor == 0x00)
//        return EV3;
    else if (major == 0x30 && minor == 0x00)
        return LIGHT;
    else
        return UNKNOWN;
}

//none
static int test_desfire_authenticate() {
    uint8_t c = 0x00;
    sAPDU apdu = {0x90, MFDES_AUTHENTICATE, 0x00, 0x00, 0x01, &c}; // 0x0A, KEY 0
    int recv_len = 0;
    uint16_t sw = 0;
    return send_desfire_cmd(&apdu, false, NULL, &recv_len, &sw, 0);
}

// none
static int test_desfire_authenticate_iso() {
    uint8_t c = 0x00;
    sAPDU apdu = {0x90, MFDES_AUTHENTICATE_ISO, 0x00, 0x00, 0x01, &c}; // 0x1A, KEY 0
    int recv_len = 0;
    uint16_t sw = 0;
    return send_desfire_cmd(&apdu, false, NULL, &recv_len, &sw, 0);
}

//none
static int test_desfire_authenticate_aes() {
    uint8_t c = 0x00;
    sAPDU apdu = {0x90, MFDES_AUTHENTICATE_AES, 0x00, 0x00, 0x01, &c}; // 0xAA, KEY 0
    int recv_len = 0;
    uint16_t sw = 0;
    return send_desfire_cmd(&apdu, false, NULL, &recv_len, &sw, 0);
}

// --- FREE MEM
static int desfire_print_freemem(uint32_t free_mem) {
    PrintAndLogEx(SUCCESS, "   Available free memory on card         : " _GREEN_("%d bytes"), free_mem);
    return PM3_SUCCESS;
}

// init / disconnect
static int get_desfire_freemem(uint32_t *free_mem) {
    sAPDU apdu = {0x90, MFDES_GET_FREE_MEMORY, 0x00, 0x00, 0x00, NONE}; // 0x6E
    int recv_len = 0;
    uint16_t sw = 0;
    uint8_t fmem[4] = {0};

    int res = send_desfire_cmd(&apdu, true, fmem, &recv_len, &sw, 0);
    if (res == PM3_SUCCESS) {
        *free_mem = le24toh(fmem);
        return res;
    }
    *free_mem = 0;
    return res;
}


// --- GET SIGNATURE
static int desfire_print_signature(uint8_t *uid, uint8_t *signature, size_t signature_len, desfire_cardtype_t card_type) {

    // DESFire Ev3  - wanted
    // ref:  MIFARE Desfire Originality Signature Validation

#define PUBLIC_DESFIRE_ECDA_KEYLEN 57
    const ecdsa_publickey_t nxp_desfire_public_keys[] = {
        {"NTAG424DNA, DESFire EV2", "048A9B380AF2EE1B98DC417FECC263F8449C7625CECE82D9B916C992DA209D68422B81EC20B65A66B5102A61596AF3379200599316A00A1410"},
        {"NTAG413DNA, DESFire EV1", "04BB5D514F7050025C7D0F397310360EEC91EAF792E96FC7E0F496CB4E669D414F877B7B27901FE67C2E3B33CD39D1C797715189AC951C2ADD"},
        {"DESFire EV2",             "04B304DC4C615F5326FE9383DDEC9AA892DF3A57FA7FFB3276192BC0EAA252ED45A865E3B093A3D0DCE5BE29E92F1392CE7DE321E3E5C52B3A"},
        {"NTAG424DNA, NTAG424DNATT, DESFire Light EV2", "04B304DC4C615F5326FE9383DDEC9AA892DF3A57FA7FFB3276192BC0EAA252ED45A865E3B093A3D0DCE5BE29E92F1392CE7DE321E3E5C52B3B"},
        {"DESFire Light EV1",       "040E98E117AAA36457F43173DC920A8757267F44CE4EC5ADD3C54075571AEBBF7B942A9774A1D94AD02572427E5AE0A2DD36591B1FB34FCF3D"},
        {"Mifare Plus EV1",         "044409ADC42F91A8394066BA83D872FB1D16803734E911170412DDF8BAD1A4DADFD0416291AFE1C748253925DA39A5F39A1C557FFACD34C62E"}
    };

    uint8_t i;
    int res;
    bool is_valid = false;

    for (i = 0; i < ARRAYLEN(nxp_desfire_public_keys); i++) {

        int dl = 0;
        uint8_t key[PUBLIC_DESFIRE_ECDA_KEYLEN];
        param_gethex_to_eol(nxp_desfire_public_keys[i].value, 0, key, PUBLIC_DESFIRE_ECDA_KEYLEN, &dl);

        res = ecdsa_signature_r_s_verify(MBEDTLS_ECP_DP_SECP224R1, key, uid, 7, signature, signature_len, false);
        is_valid = (res == 0);
        if (is_valid)
            break;
    }
    if (is_valid == false) {
        PrintAndLogEx(SUCCESS, "Signature verification " _RED_("failed"));
        return PM3_ESOFT;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Tag Signature"));
    PrintAndLogEx(INFO, " IC signature public key name: " _GREEN_("%s"), nxp_desfire_public_keys[i].desc);
    PrintAndLogEx(INFO, "IC signature public key value: %.32s", nxp_desfire_public_keys[i].value);
    PrintAndLogEx(INFO, "                             : %.32s", nxp_desfire_public_keys[i].value + 16);
    PrintAndLogEx(INFO, "                             : %.32s", nxp_desfire_public_keys[i].value + 32);
    PrintAndLogEx(INFO, "                             : %.32s", nxp_desfire_public_keys[i].value + 48);
    PrintAndLogEx(INFO, "    Elliptic curve parameters: NID_secp224r1");
    PrintAndLogEx(INFO, "             TAG IC Signature: %s", sprint_hex_inrow(signature, 16));
    PrintAndLogEx(INFO, "                             : %s", sprint_hex_inrow(signature + 16, 16));
    PrintAndLogEx(INFO, "                             : %s", sprint_hex_inrow(signature + 32, 16));
    PrintAndLogEx(INFO, "                             : %s", sprint_hex_inrow(signature + 48, signature_len - 48));
    PrintAndLogEx(SUCCESS, "           Signature verified: " _GREEN_("successful"));
    return PM3_SUCCESS;
}

// init / disconnect
static int get_desfire_signature(uint8_t *signature, size_t *signature_len) {
    uint8_t c = 0x00;
    sAPDU apdu = {0x90, MFDES_READSIG, 0x00, 0x00, 0x01, &c}; // 0x3C
    int recv_len = 0;
    uint16_t sw = 0;
    int res = send_desfire_cmd(&apdu, true, signature, &recv_len, &sw, 0);
    if (res == PM3_SUCCESS) {
        if (recv_len != 56) {
            *signature_len = 0;
            DropField();
            return PM3_ESOFT;
        } else {
            *signature_len = recv_len;

        }
        DropField();
        return PM3_SUCCESS;
    }
    DropField();
    return res;
}


// --- KEY SETTING
static int desfire_print_keysetting(uint8_t key_settings, uint8_t num_keys) {

    PrintAndLogEx(SUCCESS, "  AID Key settings           : %02x", key_settings);
    PrintAndLogEx(SUCCESS, "  Max number of keys in AID  : %d", num_keys);
    PrintAndLogEx(INFO, "-------------------------------------------------------------");
    PrintAndLogEx(SUCCESS, "  Changekey Access rights");

    // Access rights.
    uint8_t rights = (key_settings >> 4 & 0x0F);
    switch (rights) {
        case 0x0:
            PrintAndLogEx(SUCCESS, "  -- AMK authentication is necessary to change any key (default)");
            break;
        case 0xE:
            PrintAndLogEx(SUCCESS, "  -- Authentication with the key to be changed (same KeyNo) is necessary to change a key");
            break;
        case 0xF:
            PrintAndLogEx(SUCCESS, "  -- All keys (except AMK,see Bit0) within this application are frozen");
            break;
        default:
            PrintAndLogEx(SUCCESS, "  -- Authentication with the specified key is necessary to change any key.\nA change key and a PICC master key (CMK) can only be changed after authentication with the master key.\nFor keys other then the master or change key, an authentication with the same key is needed.");
            break;
    }

    PrintAndLogEx(SUCCESS, "   [0x08] Configuration changeable       : %s", (key_settings & (1 << 3)) ? _GREEN_("YES") : "NO");
    PrintAndLogEx(SUCCESS, "   [0x04] AMK required for create/delete : %s", (key_settings & (1 << 2)) ? "NO" : "YES");
    PrintAndLogEx(SUCCESS, "   [0x02] Directory list access with AMK : %s", (key_settings & (1 << 1)) ? "NO" : "YES");
    PrintAndLogEx(SUCCESS, "   [0x01] AMK is changeable              : %s", (key_settings & (1 << 0)) ? _GREEN_("YES") : "NO");
    return PM3_SUCCESS;
}

// none
static int get_desfire_keysettings(uint8_t *key_settings, uint8_t *num_keys) {
    sAPDU apdu = {0x90, MFDES_GET_KEY_SETTINGS, 0x00, 0x00, 0x00, NONE}; //0x45
    int recv_len = 0;
    uint16_t sw = 0;
    uint8_t data[2] = {0};
    if (num_keys == NULL) return PM3_ESOFT;
    if (key_settings == NULL) return PM3_ESOFT;
    int res = send_desfire_cmd(&apdu, false, data, &recv_len, &sw, 0);
    if (sw == MFDES_EAUTH_RESP) {
        PrintAndLogEx(WARNING, _RED_("[get_desfire_keysettings] Authentication error"));
        return PM3_ESOFT;
    }
    if (res != PM3_SUCCESS) return res;

    *key_settings = data[0];
    *num_keys = data[1];
    return PM3_SUCCESS;
}

// --- KEY VERSION
static int desfire_print_keyversion(uint8_t key_idx, uint8_t key_version) {
    PrintAndLogEx(SUCCESS, "   Key [%u]  Version : %d (0x%02x)", key_idx, key_version, key_version);
    return PM3_SUCCESS;
}

// none
static int get_desfire_keyversion(uint8_t curr_key, uint8_t *num_versions) {
    sAPDU apdu = {0x90, MFDES_GET_KEY_VERSION, 0x00, 0x00, 0x01, &curr_key}; //0x64
    int recv_len = 0;
    uint16_t sw = 0;
    if (num_versions == NULL) return PM3_ESOFT;
    int res = send_desfire_cmd(&apdu, false, num_versions, &recv_len, &sw, 0);
    if (sw == MFDES_ENO_SUCH_KEY_RESP) {
        PrintAndLogEx(WARNING, _RED_("[get_desfire_keyversion] Key %d doesn't exist"), curr_key);
        return PM3_ESOFT;
    }
    return res;
}


// init / disconnect
static int get_desfire_appids(uint8_t *dest, uint8_t *app_ids_len) {
    sAPDU apdu = {0x90, MFDES_GET_APPLICATION_IDS, 0x00, 0x00, 0x00, NULL}; //0x6a
    int recv_len = 0;
    uint16_t sw = 0;
    if (dest == NULL) return PM3_ESOFT;
    if (app_ids_len == NULL) return PM3_ESOFT;
    int res = send_desfire_cmd(&apdu, true, dest, &recv_len, &sw, 0);
    if (res != PM3_SUCCESS) return res;
    *app_ids_len = (uint8_t)recv_len & 0xFF;
    return res;
}

static int get_desfire_dfnames(dfname_t *dest, uint8_t *dfname_count) {
    sAPDU apdu = {0x90, MFDES_GET_DF_NAMES, 0x00, 0x00, 0x00, NULL}; //0x6d
    int recv_len = 0;
    uint16_t sw = 0;
    if (dest == NULL) return PM3_ESOFT;
    if (dfname_count == NULL) return PM3_ESOFT;
    int res = send_desfire_cmd(&apdu, true, (uint8_t *)dest, &recv_len, &sw, sizeof(dfname_t));
    if (res != PM3_SUCCESS) return res;
    *dfname_count = recv_len;
    return res;
}


// init
static int get_desfire_select_application(uint8_t *aid) {
    sAPDU apdu = {0x90, MFDES_SELECT_APPLICATION, 0x00, 0x00, 0x03, aid}; //0x5a
    int recv_len = 0;
    uint16_t sw = 0;
    if (aid == NULL) return PM3_ESOFT;
    return send_desfire_cmd(&apdu, true, NULL, &recv_len, &sw, sizeof(dfname_t));
}

// none
static int get_desfire_fileids(uint8_t *dest, uint8_t *file_ids_len) {
    sAPDU apdu = {0x90, MFDES_GET_FILE_IDS, 0x00, 0x00, 0x00, NULL}; //0x6f
    int recv_len = 0;
    uint16_t sw = 0;
    if (dest == NULL) return PM3_ESOFT;
    if (file_ids_len == NULL) return PM3_ESOFT;
    *file_ids_len = 0;
    int res = send_desfire_cmd(&apdu, false, dest, &recv_len, &sw, 0);
    if (res != PM3_SUCCESS) return res;
    *file_ids_len = recv_len;
    return res;
}

static int get_desfire_filesettings(uint8_t file_id, uint8_t *dest, int *destlen) {
    sAPDU apdu = {0x90, MFDES_GET_FILE_SETTINGS, 0x00, 0x00, 0x01, &file_id}; // 0xF5
    uint16_t sw = 0;
    return send_desfire_cmd(&apdu, false, dest, destlen, &sw, 0);
}

static int CmdHF14ADesInfo(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far

    SendCommandNG(CMD_HF_DESFIRE_INFO, NULL, 0);
    PacketResponseNG resp;

    if (!WaitForResponseTimeout(CMD_HF_DESFIRE_INFO, &resp, 1500)) {
        PrintAndLogEx(WARNING, "Command execute timeout");
        DropField();
        return PM3_ETIMEOUT;
    }

    struct p {
        uint8_t isOK;
        uint8_t uid[7];
        uint8_t versionHW[7];
        uint8_t versionSW[7];
        uint8_t details[14];
    } PACKED;

    struct p *package = (struct p *) resp.data.asBytes;

    if (resp.status != PM3_SUCCESS) {

        switch (package->isOK) {
            case 1:
                PrintAndLogEx(WARNING, "Can't select card");
                break;
            case 2:
                PrintAndLogEx(WARNING, "Card is most likely not Desfire. Its UID has wrong size");
                break;
            case 3:
            default:
                PrintAndLogEx(WARNING, _RED_("Command unsuccessful"));
                break;
        }
        return PM3_ESOFT;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Tag Information") "---------------------------");
    PrintAndLogEx(INFO, "-------------------------------------------------------------");
    PrintAndLogEx(SUCCESS, "              UID: " _GREEN_("%s"), sprint_hex(package->uid, sizeof(package->uid)));
    PrintAndLogEx(SUCCESS, "     Batch number: " _GREEN_("%s"), sprint_hex(package->details + 7, 5));
    PrintAndLogEx(SUCCESS, "  Production date: week " _GREEN_("%02x") "/ " _GREEN_("20%02x"), package->details[12], package->details[13]);
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Hardware Information"));
    PrintAndLogEx(INFO, "     Vendor Id: " _YELLOW_("%s"), getTagInfo(package->versionHW[0]));
    PrintAndLogEx(INFO, "          Type: " _YELLOW_("0x%02X"), package->versionHW[1]);
    PrintAndLogEx(INFO, "       Subtype: " _YELLOW_("0x%02X"), package->versionHW[2]);
    PrintAndLogEx(INFO, "       Version: %s", getVersionStr(package->versionHW[3], package->versionHW[4]));
    PrintAndLogEx(INFO, "  Storage size: %s", getCardSizeStr(package->versionHW[5]));
    PrintAndLogEx(INFO, "      Protocol: %s", getProtocolStr(package->versionHW[6]));
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Software Information"));
    PrintAndLogEx(INFO, "     Vendor Id: " _YELLOW_("%s"), getTagInfo(package->versionSW[0]));
    PrintAndLogEx(INFO, "          Type: " _YELLOW_("0x%02X"), package->versionSW[1]);
    PrintAndLogEx(INFO, "       Subtype: " _YELLOW_("0x%02X"), package->versionSW[2]);
    PrintAndLogEx(INFO, "       Version: " _YELLOW_("%d.%d"),  package->versionSW[3], package->versionSW[4]);
    PrintAndLogEx(INFO, "  Storage size: %s", getCardSizeStr(package->versionSW[5]));
    PrintAndLogEx(INFO, "      Protocol: %s", getProtocolStr(package->versionSW[6]));

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Card capabilities"));
    uint8_t major = package->versionSW[3];
    uint8_t minor = package->versionSW[4];
    if (major == 0 && minor == 4)
        PrintAndLogEx(INFO, "\t0.4 - DESFire MF3ICD40, No support for APDU (only native commands)");
    if (major == 0 && minor == 5)
        PrintAndLogEx(INFO, "\t0.5 - DESFire MF3ICD40, Support for wrapping commands inside ISO 7816 style APDUs");
    if (major == 0 && minor == 6)
        PrintAndLogEx(INFO, "\t0.6 - DESFire MF3ICD40, Add ISO/IEC 7816 command set compatibility");
    if (major == 1 && minor == 3)
        PrintAndLogEx(INFO, "\t1.3 - DESFire Ev1 MF3ICD21/41/81, Support extended APDU commands, EAL4+");
    if (major == 1 && minor == 4)
        PrintAndLogEx(INFO, "\t1.4 - DESFire Ev1 MF3ICD21/41/81, EAL4+, N/A (report to iceman!)");
    if (major == 2 && minor == 0)
        PrintAndLogEx(INFO, "\t2.0 - DESFire Ev2, Originality check, proximity check, EAL5");
//    if (major == 3 && minor == 0)
//        PrintAndLogEx(INFO, "\t3.0 - DESFire Ev3, Originality check, proximity check, badass EAL5");

    if (major == 0 && minor == 2)
        PrintAndLogEx(INFO, "\t0.2 - DESFire Light, Originality check, ");

    // Signature originality check
    uint8_t signature[56] = {0};
    size_t signature_len = 0;
    desfire_cardtype_t cardtype = getCardType(package->versionHW[3], package->versionHW[4]);

    if (get_desfire_signature(signature, &signature_len) == PM3_SUCCESS)
        desfire_print_signature(package->uid, signature, signature_len, cardtype);

    // Master Key settings
    uint8_t master_aid[3] = {0x00, 0x00, 0x00};
    getKeySettings(master_aid);

    // Free memory on card
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Free memory"));
    uint32_t free_mem = 0;
    if (get_desfire_freemem(&free_mem) == PM3_SUCCESS) {
        desfire_print_freemem(free_mem);
    } else {
        PrintAndLogEx(SUCCESS, "   Card doesn't support 'free mem' cmd");
    }
    PrintAndLogEx(INFO, "-------------------------------------------------------------");

    /*
        Card Master key (CMK)        0x00 AID = 00 00 00 (card level)
        Application Master Key (AMK) 0x00 AID != 00 00 00
        Application keys (APK)       0x01-0x0D
        Application free             0x0E
        Application never            0x0F

        ACCESS RIGHTS:
        keys 0,1,2,3     C
        keys 4,5,6,7     RW
        keys 8,9,10,11   W
        keys 12,13,14,15 R

    */

    DropField();
    return PM3_SUCCESS;
}

/*
  The 7 MSBits (= n) code the storage size itself based on 2^n,
  the LSBit is set to '0' if the size is exactly 2^n
    and set to '1' if the storage size is between 2^n and 2^(n+1).
    For this version of DESFire the 7 MSBits are set to 0x0C (2^12 = 4096) and the LSBit is '0'.
*/
char *getCardSizeStr(uint8_t fsize) {

    static char buf[40] = {0x00};
    char *retStr = buf;

    uint16_t usize = 1 << ((fsize >> 1) + 1);
    uint16_t lsize = 1 << (fsize >> 1);

    // is  LSB set?
    if (fsize & 1)
        sprintf(retStr, "0x%02X ( " _YELLOW_("%d - %d bytes") ")", fsize, usize, lsize);
    else
        sprintf(retStr, "0x%02X ( " _YELLOW_("%d bytes") ")", fsize, lsize);
    return buf;
}

char *getProtocolStr(uint8_t id) {

    static char buf[40] = {0x00};
    char *retStr = buf;

    if (id == 0x05)
        sprintf(retStr, "0x%02X ( " _YELLOW_("ISO 14443-3, 14443-4") ")", id);
    else
        sprintf(retStr, "0x%02X ( " _YELLOW_("Unknown") ")", id);
    return buf;
}

char *getVersionStr(uint8_t major, uint8_t minor) {

    static char buf[40] = {0x00};
    char *retStr = buf;

    if (major == 0x00)
        sprintf(retStr, "%x.%x ( " _YELLOW_("DESFire MF3ICD40") ")", major, minor);
    else if (major == 0x01 && minor == 0x00)
        sprintf(retStr, "%x.%x ( " _YELLOW_("DESFire EV1") ")", major, minor);
    else if (major == 0x12 && minor == 0x00)
        sprintf(retStr, "%x.%x ( " _YELLOW_("DESFire EV2") ")", major, minor);
//    else if (major == 0x13 && minor == 0x00)
//        sprintf(retStr, "%x.%x ( " _YELLOW_("DESFire EV3") ")", major, minor);
    else if (major == 0x30 && minor == 0x00)
        sprintf(retStr, "%x.%x ( " _YELLOW_("DESFire Light") ")", major, minor);
    else
        sprintf(retStr, "%x.%x ( " _YELLOW_("Unknown") ")", major, minor);
    return buf;
}

void getKeySettings(uint8_t *aid) {

    if (memcmp(aid, "\x00\x00\x00", 3) == 0) {

        // CARD MASTER KEY
        //PrintAndLogEx(INFO, "--- " _CYAN_("CMK - PICC, Card Master Key settings"));
        if (get_desfire_select_application(aid) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, _RED_("   Can't select AID"));
            DropField();
            return;
        }

        // KEY Settings - AMK
        uint8_t num_keys = 0;
        uint8_t key_setting = 0;
        if (get_desfire_keysettings(&key_setting, &num_keys) == PM3_SUCCESS) {
            // number of Master keys (0x01)
            PrintAndLogEx(SUCCESS, "   Number of Masterkeys                  : " _YELLOW_("%u"), (num_keys & 0x3F));

            PrintAndLogEx(SUCCESS, "   [0x08] Configuration changeable       : %s", (key_setting & (1 << 3)) ? _GREEN_("YES") : "NO");
            PrintAndLogEx(SUCCESS, "   [0x04] CMK required for create/delete : %s", (key_setting & (1 << 2)) ? _GREEN_("YES") : "NO");
            PrintAndLogEx(SUCCESS, "   [0x02] Directory list access with CMK : %s", (key_setting & (1 << 1)) ? _GREEN_("YES") : "NO");
            PrintAndLogEx(SUCCESS, "   [0x01] CMK is changeable              : %s", (key_setting & (1 << 0)) ? _GREEN_("YES") : "NO");
        } else {
            PrintAndLogEx(WARNING, _RED_("   Can't read Application Master key settings"));
        }

        const char *str = "   Operation of PICC master key          : " _YELLOW_("%s");

        // 2 MSB denotes
        switch (num_keys >> 6) {
            case 0:
                PrintAndLogEx(SUCCESS, str, "(3)DES");
                break;
            case 1:
                PrintAndLogEx(SUCCESS, str, "3K3DES");
                break;
            case 2:
                PrintAndLogEx(SUCCESS, str, "AES");
                break;
            default:
                break;
        }

        uint8_t cmk_num_versions = 0;
        if (get_desfire_keyversion(0, &cmk_num_versions) == PM3_SUCCESS) {
            PrintAndLogEx(SUCCESS, "   PICC Master key Version               : " _YELLOW_("%d (0x%02x)"), cmk_num_versions, cmk_num_versions);
            PrintAndLogEx(INFO, "   ----------------------------------------------------------");
        }

        // Authentication tests
        int res = test_desfire_authenticate();
        if (res == PM3_ETIMEOUT) return;
        PrintAndLogEx(SUCCESS, "   [0x0A] Authenticate      : %s", (res == PM3_SUCCESS) ? _YELLOW_("YES") : "NO");

        res = test_desfire_authenticate_iso();
        if (res == PM3_ETIMEOUT) return;
        PrintAndLogEx(SUCCESS, "   [0x1A] Authenticate ISO  : %s", (res == PM3_SUCCESS) ? _YELLOW_("YES") : "NO");

        res = test_desfire_authenticate_aes();
        if (res == PM3_ETIMEOUT) return;
        PrintAndLogEx(SUCCESS, "   [0xAA] Authenticate AES  : %s", (res == PM3_SUCCESS) ? _YELLOW_("YES") : "NO");

        PrintAndLogEx(INFO, "-------------------------------------------------------------");

    } else {

        // AID - APPLICATION MASTER KEYS
        //PrintAndLogEx(SUCCESS, "--- " _CYAN_("AMK - Application Master Key settings"));
        if (get_desfire_select_application(aid) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, _RED_("   Can't select AID"));
            DropField();
            return;
        }

        // KEY Settings - AMK
        uint8_t num_keys = 0;
        uint8_t key_setting = 0;
        if (get_desfire_keysettings(&key_setting, &num_keys) == PM3_SUCCESS) {
            desfire_print_keysetting(key_setting, num_keys);
        } else {
            PrintAndLogEx(WARNING, _RED_("   Can't read Application Master key settings"));
        }

        // KEY VERSION  - AMK
        uint8_t num_version = 0;
        if (get_desfire_keyversion(0, &num_version) == PM3_SUCCESS) {
            PrintAndLogEx(INFO, "-------------------------------------------------------------");
            PrintAndLogEx(INFO, "  Application keys");
            desfire_print_keyversion(0, num_version);
        } else {
            PrintAndLogEx(WARNING, "   Can't read AID master key version. Trying all keys");
        }

        // From 0x01 to numOfKeys.  We already got 0x00. (AMK)
        num_keys &= 0x3F;
        if (num_keys > 1) {
            for (uint8_t i = 0x01; i < num_keys; ++i) {
                if (get_desfire_keyversion(i, &num_version) == PM3_SUCCESS) {
                    desfire_print_keyversion(i, num_version);
                } else {
                    PrintAndLogEx(WARNING, "   Can't read key %d  (0x%02x) version", i, i);
                }
            }
        }
    }

    DropField();
}

static int CmdHF14ADesEnumApplications(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far

//    uint8_t isOK = 0x00;
    uint8_t aid[3] = {0};
    uint8_t app_ids[78] = {0};
    uint8_t app_ids_len = 0;

    uint8_t file_ids[33] = {0};
    uint8_t file_ids_len = 0;

    dfname_t dfnames[255] = {0};
    uint8_t dfname_count = 0;

    if (get_desfire_appids(app_ids, &app_ids_len) != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "Can't get list of applications on tag");
        DropField();
        return PM3_ESOFT;
    }

    if (get_desfire_dfnames(dfnames, &dfname_count) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, _RED_("Can't get DF Names"));
        DropField();
        return PM3_ESOFT;
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "-- Mifare DESFire Enumerate applications --------------------");
    PrintAndLogEx(INFO, "-------------------------------------------------------------");
    PrintAndLogEx(SUCCESS, " Tag report " _GREEN_("%d") "application%c", app_ids_len / 3, (app_ids_len == 3) ? ' ' : 's');

    for (int i = 0; i < app_ids_len; i += 3) {

        aid[0] = app_ids[i];
        aid[1] = app_ids[i + 1];
        aid[2] = app_ids[i + 2];

        PrintAndLogEx(NORMAL, "");

        if (memcmp(aid, "\x00\x00\x00", 3) == 0) {
            // CARD MASTER KEY
            PrintAndLogEx(INFO, "--- " _CYAN_("CMK - PICC, Card Master Key settings"));
        } else {
            PrintAndLogEx(SUCCESS, "--- " _CYAN_("AMK - Application Master Key settings"));
        }

        PrintAndLogEx(SUCCESS, "  AID : " _GREEN_("%02X %02X %02X"), aid[0], aid[1], aid[2]);
        for (int m = 0; m < dfname_count; m++) {
            if (dfnames[m].aid[0] == aid[0] && dfnames[m].aid[1] == aid[1] && dfnames[m].aid[2] == aid[2]) {
                PrintAndLogEx(SUCCESS, "  -  DF " _YELLOW_("%02X %02X") " Name : " _YELLOW_("%s"), dfnames[m].fid[0], dfnames[m].fid[1], dfnames[m].name);
            }
        }

        getKeySettings(aid);


        if (get_desfire_select_application(aid) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, _RED_("   Can't select AID"));
            DropField();
            return PM3_ESOFT;
        }

        // Get File IDs
        if (get_desfire_fileids(file_ids, &file_ids_len) == PM3_SUCCESS) {
            PrintAndLogEx(SUCCESS, " Tag report " _GREEN_("%d") "file%c", file_ids_len, (file_ids_len == 1) ? ' ' : 's');
            for (int j = 0; j < file_ids_len; ++j) {
                PrintAndLogEx(SUCCESS, "   Fileid %d (0x%02x)", file_ids[j], file_ids[j]);

                uint8_t filesettings[20] = {0};
                int fileset_len = 0;
                int res = get_desfire_filesettings(j, filesettings, &fileset_len);
                if (res == PM3_SUCCESS) {
                    PrintAndLogEx(INFO, "  Settings [%u] %s", fileset_len, sprint_hex(filesettings, fileset_len));
                }
            }
        }




        /*
                // Get ISO File IDs
                {
                    uint8_t data[] = {GET_ISOFILE_IDS, 0x00, 0x00, 0x00};  // 0x61
                    SendCommandMIX(CMD_HF_DESFIRE_COMMAND, DISCONNECT, sizeof(data), 0, data, sizeof(data));
                }

                if (!WaitForResponseTimeout(CMD_ACK, &respFiles, 1500)) {
                    PrintAndLogEx(WARNING, _RED_("   Timed-out"));
                    continue;
                } else {
                    isOK  = respFiles.data.asBytes[2] & 0xff;
                    if (!isOK) {
                        PrintAndLogEx(WARNING, _RED_("   Can't get ISO file ids"));
                    } else {
                        int respfileLen = resp.oldarg[1] - 3 - 2;
                        for (int j = 0; j < respfileLen; ++j) {
                            PrintAndLogEx(SUCCESS, " ISO  Fileid %d :", resp.data.asBytes[j + 3]);
                        }
                    }
                }
                */
    }
    PrintAndLogEx(INFO, "-------------------------------------------------------------");
    DropField();
    return PM3_SUCCESS;
}

// MIAFRE DESFire Authentication
//
#define BUFSIZE 256
static int CmdHF14ADesAuth(const char *Cmd) {
    clearCommandBuffer();
    // NR  DESC     KEYLENGHT
    // ------------------------
    // 1 = DES      8
    // 2 = 3DES     16
    // 3 = 3K 3DES  24
    // 4 = AES      16
    //SetAPDULogging(true);
    uint8_t keylength = 8;

    CLIParserInit("hf mfdes auth",
                  "Authenticates Mifare DESFire using Key",
                  "Usage:\n\t-m Auth type (1=normal, 2=iso, 3=aes)\n\t-t Crypt algo (1=DES, 2=3DES, 3=3K3DES, 4=aes)\n\t-a aid (3 bytes)\n\t-n keyno\n\t-k key (8-24 bytes)\n\n"
                  "Example:\n\thf mfdes auth -m 3 -t 4 -a 018380 -n 0 -k 404142434445464748494a4b4c4d4e4f\n"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_int0("mM",  "type",   "Auth type (1=normal, 2=iso, 3=aes)", NULL),
        arg_int0("tT",  "algo",   "Crypt algo (1=DES, 2=3DES, 3=3K3DES, 4=aes)", NULL),
        arg_strx0("aA",  "aid",    "<aid>", "AID used for authentification"),
        arg_int0("nN",  "keyno",  "Key number used for authentification", NULL),
        arg_str0("kK",  "key",     "<Key>", "Key for checking (HEX 16 bytes)"),
        arg_param_end
    };
    CLIExecWithReturn(Cmd, argtable, true);

    uint8_t cmdAuthMode = arg_get_int_def(1, 0);
    uint8_t cmdAuthAlgo = arg_get_int_def(2, 0);

    int aidlength = 3;
    uint8_t aid[3] = {0};
    CLIGetHexWithReturn(3, aid, &aidlength);

    uint8_t cmdKeyNo  = arg_get_int_def(4, 0);

    uint8_t key[24] = {0};
    int keylen = 0;
    CLIGetHexWithReturn(5, key, &keylen);
    CLIParserFree();

    if ((keylen < 8) || (keylen > 24)) {
        PrintAndLogEx(ERR, "Specified key must have 16 bytes length.");
        //SetAPDULogging(false);
        return PM3_EINVARG;
    }

    // AID
    if (aidlength != 3) {
        PrintAndLogEx(WARNING, "aid must include %d HEX symbols", 3);
        //SetAPDULogging(false);
        return PM3_EINVARG;
    }

    switch (cmdAuthMode) {
        case 1:
            if (cmdAuthAlgo != 1 && cmdAuthAlgo != 2) {
                PrintAndLogEx(NORMAL, "Crypto algo not valid for the auth mode");
                //SetAPDULogging(false);
                return PM3_EINVARG;
            }
            break;
        case 2:
            if (cmdAuthAlgo != 1 && cmdAuthAlgo != 2 && cmdAuthAlgo != 3) {
                PrintAndLogEx(NORMAL, "Crypto algo not valid for the auth mode");
                //SetAPDULogging(false);
                return PM3_EINVARG;
            }
            break;
        case 3:
            if (cmdAuthAlgo != 4) {
                PrintAndLogEx(NORMAL, "Crypto algo not valid for the auth mode");
                //SetAPDULogging(false);
                return PM3_EINVARG;
            }
            break;
        default:
            PrintAndLogEx(WARNING, "Wrong Auth mode (%d) -> (1=normal, 2=iso, 3=aes)", cmdAuthMode);
            //SetAPDULogging(false);
            return PM3_EINVARG;
    }

    switch (cmdAuthAlgo) {
        case 2:
            keylength = 16;
            PrintAndLogEx(NORMAL, "3DES selected");
            break;
        case 3:
            keylength = 24;
            PrintAndLogEx(NORMAL, "3 key 3DES selected");
            break;
        case 4:
            keylength = 16;
            PrintAndLogEx(NORMAL, "AES selected");
            break;
        default:
            cmdAuthAlgo = 1;
            keylength = 8;
            PrintAndLogEx(NORMAL, "DES selected");
            break;
    }

    // KEY
    if (keylen != keylength) {
        PrintAndLogEx(WARNING, "Key must include %d HEX symbols", keylength);
        return PM3_EINVARG;
    }

    if (get_desfire_select_application(aid) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, _RED_("   Can't select AID"));
        DropField();
        return PM3_ESOFT;
    }

    uint8_t file_ids[33] = {0};
    uint8_t file_ids_len = 0;
    int res = get_desfire_fileids(file_ids, &file_ids_len);
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "Get file ids error.");
        DropField();
        return res;
    }


    // algo, keylength,
    uint8_t data[25] = {keylength}; // max length: 1 + 24 (3k3DES)
    memcpy(data + 1, key, keylength);
    SendCommandOLD(CMD_HF_DESFIRE_AUTH1, cmdAuthMode, cmdAuthAlgo, cmdKeyNo, data, keylength + 1);
    PacketResponseNG resp;

    if (!WaitForResponseTimeout(CMD_ACK, &resp, 3000)) {
        PrintAndLogEx(WARNING, "Client command execute timeout");
        DropField();
        return PM3_ETIMEOUT;
    }

    uint8_t isOK  = resp.oldarg[0] & 0xff;
    if (isOK) {
        uint8_t *session_key = resp.data.asBytes;

        PrintAndLogEx(SUCCESS, "  Key        : " _GREEN_("%s"), sprint_hex(key, keylength));
        PrintAndLogEx(SUCCESS, "  SESSION    : " _GREEN_("%s"), sprint_hex(session_key, keylength));
        PrintAndLogEx(INFO, "-------------------------------------------------------------");
        //PrintAndLogEx(NORMAL, "  Expected   :B5 21 9E E8 1A A7 49 9D 21 96 68 7E 13 97 38 56");
    } else {
        PrintAndLogEx(WARNING, _RED_("Client command failed."));
    }
    PrintAndLogEx(INFO, "-------------------------------------------------------------");
    return PM3_SUCCESS;
}

static int CmdHF14ADesList(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far
    return CmdTraceList("des");
}

static command_t CommandTable[] = {
    {"help",    CmdHelp,                     AlwaysAvailable, "This help"},
    {"info",    CmdHF14ADesInfo,             IfPm3Iso14443a,  "Tag information"},
    {"list",    CmdHF14ADesList,             AlwaysAvailable, "List DESFire (ISO 14443A) history"},
    {"enum",    CmdHF14ADesEnumApplications, IfPm3Iso14443a,  "Tries enumerate all applications"},
    {"auth",    CmdHF14ADesAuth,             IfPm3Iso14443a,  "Tries a MIFARE DesFire Authentication"},
//    {"rdbl",    CmdHF14ADesRb,               IfPm3Iso14443a,  "Read MIFARE DesFire block"},
//    {"wrbl",    CmdHF14ADesWb,               IfPm3Iso14443a,  "write MIFARE DesFire block"},
    {NULL, NULL, NULL, NULL}
};

static int CmdHelp(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far
    CmdsHelp(CommandTable);
    return PM3_SUCCESS;
}

int CmdHFMFDes(const char *Cmd) {
    // flush
    clearCommandBuffer();
    return CmdsParse(CommandTable, Cmd);
}
