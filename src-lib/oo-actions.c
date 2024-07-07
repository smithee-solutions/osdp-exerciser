/*
  oo-actions - open osdp action routines

  (C)Copyright 2017-2024 Smithee Solutions LLC

  Support provided by the Security Industry Association
  http://www.securityindustry.org

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at
 
    http://www.apache.org/licenses/LICENSE-2.0
 
  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/


#include <stdio.h>
#include <memory.h>
#include <stdlib.h>
#include <unistd.h>


#include <aes.h>


#include <osdp-tls.h>
#include <open-osdp.h>
#include <osdp_conformance.h>


extern OSDP_INTEROP_ASSESSMENT osdp_conformance;
extern OSDP_PARAMETERS p_card;


// used for responses to osdp_POLL

int pending_response_length;
unsigned char pending_response_data [1500];
unsigned char pending_response;


int
  action_osdp_COMSET
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_COMSET */

  int current_length;
  unsigned char from_address;
  int i;
  unsigned char new_addr;
  unsigned char osdp_com_response_data [5];
  char logmsg [1024];
  OSDP_HDR *p;
  int status;


  status = oosdp_callout(ctx, "osdp_COMSET", "");
  p = (OSDP_HDR *)(msg->ptr);
  from_address = p->addr;
  memset (osdp_com_response_data, 0, sizeof (osdp_com_response_data));
  i = *(1+msg->data_payload) + (*(2+msg->data_payload) << 8) +
    (*(3+msg->data_payload) << 16) + (*(4+msg->data_payload) << 24);

  sprintf(logmsg, "COMSET Data Payload %02x %02x%02x%02x%02x %d. 0x%x",
    *(0+msg->data_payload), *(1+msg->data_payload),
    *(2+msg->data_payload), *(3+msg->data_payload),
    *(4+msg->data_payload), i, i);
  fprintf(ctx->log, "%s\n", logmsg);

  new_addr = *(msg->data_payload); // first byte is new PD addr
  if ((new_addr >= 0) && (new_addr <= 0x7E))
    p_card.addr = new_addr;
  fprintf (ctx->log, "PD Address set to %02x\n", p_card.addr);

  osdp_com_response_data [0] = p_card.addr;
  if (OSDP_LOCK_SPEED)
    *(unsigned long int *)(osdp_com_response_data+1) = 9600; // hard-code to 9600 BPS
  else
    *(unsigned long int *)(osdp_com_response_data+1) = i;

  status = ST_OK;

  if (ctx->verbosity > 2)
  {
    sprintf (logmsg, "Responding with OSDP_COM");
    fprintf (ctx->log, "%s\n", logmsg); logmsg[0]=0;
  };
  if (status EQUALS ST_OK)
  {
    ctx->new_address = p_card.addr;
    sprintf(ctx->serial_speed, "%d", i);

    fprintf(ctx->log, "comset to addr %02x speed %s\n",
      p_card.addr, ctx->serial_speed);
    if (ctx->verbosity > 2)
      fprintf(ctx->log, "OSDP_COMSET received, setting addr to %02x speed to %s.\n",
        p_card.addr, ctx->serial_speed);
    (void)oo_save_parameters(ctx, OSDP_SAVED_PARAMETERS, NULL);
    status = init_serial (ctx, p_card.filename);
  };

  // send the response to the ACU

  current_length = 0;
  status = send_message_ex (ctx, OSDP_COM, oo_response_address(ctx, from_address),
    &current_length, sizeof (osdp_com_response_data), osdp_com_response_data,
    OSDP_SEC_SCS_18, 0, NULL);

  osdp_test_set_status(OOC_SYMBOL_cmd_comset, OCONFORM_EXERCISED);
  osdp_test_set_status(OOC_SYMBOL_resp_com, OCONFORM_EXERCISED);
  return (status);

} /* action_osdp_COMSET */


int
  action_osdp_FILETRANSFER
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_FILETRANSFER */

  OSDP_HDR_FILETRANSFER *filetransfer_message;
  unsigned short int fragment_size;
  unsigned int offset;
  OSDP_HDR_FTSTAT response;
  int status;
  int status_io;
  unsigned char *transfer_fragment;


  status = ST_OK;
  filetransfer_message = (OSDP_HDR_FILETRANSFER *)(msg->data_payload);
  memset (&response, 0, sizeof(response));

  if (status EQUALS ST_OK)
    status = osdp_filetransfer_validate(ctx, filetransfer_message,
      &fragment_size, &offset);
  (void)oosdp_make_message (OOSDP_MSG_FILETRANSFER, tlogmsg, msg);
  if (ctx->verbosity > 0)
  {
    fprintf(ctx->log, "%s\n", tlogmsg);
    fflush(ctx->log);
  };
// check FtType
// check FtFragmentSize sane

  if (status EQUALS ST_OK)
  {
    transfer_fragment = &(filetransfer_message->FtData);
    if (offset EQUALS 0)
    {
      ctx->xferctx.xferf = fopen("./incoming_data", "w");
      if (ctx->xferctx.xferf EQUALS NULL)
        status = ST_OSDP_BAD_TRANSFER_SAVE;
      if (status != ST_OK)
      {
        // if open of write file failed, send back error and reset

        osdp_doubleByte_to_array(OSDP_FTSTAT_ABORT_TRANSFER,
          response.FtStatusDetail);
        status = oo_send_ftstat(ctx, &response);
        (void) osdp_wrapup_filetransfer(ctx);
      };
    };
  };
  if (status EQUALS ST_OK)
  {
    status_io = fwrite(transfer_fragment, sizeof(transfer_fragment[0]),
      fragment_size, ctx->xferctx.xferf);
    if (status_io != fragment_size)
    {
      // not same error but need to abort so same status code on the wire

      osdp_doubleByte_to_array(OSDP_FTSTAT_ABORT_TRANSFER,
        response.FtStatusDetail);
      status = oo_send_ftstat(ctx, &response);
      if (ctx->verbosity > 3)
      {
        if (status != ST_OK)
          fprintf(ctx->log, "FTSTAT send for abort returned status %d.\n", status);
      };
      osdp_wrapup_filetransfer(ctx);
    }
    else
    {
      // update counters

      ctx->xferctx.current_offset = ctx->xferctx.current_offset + fragment_size;
      if (ctx->xferctx.current_offset EQUALS ctx->xferctx.total_length)
      {
        osdp_doubleByte_to_array(OSDP_FTSTAT_PROCESSED,
          response.FtStatusDetail);
        status = oo_send_ftstat(ctx, &response);
        if (ctx->verbosity > 3)
        {
          if (status != ST_OK)
            fprintf(ctx->log, "FTSTAT send at finish returned status %d.\n", status);
        };
        osdp_wrapup_filetransfer(ctx);
      }
      else
      {
        int offered_size;

        // offer an updated receive size.

        offered_size = oo_filetransfer_SDU_offer(ctx);

// was there to deal with buffer overflow issues.
#if 0
        if (offered_size > 500)
        {
          offered_size = 500;
fprintf(stderr, "DEBUG: trimmed offered size to %d.\n", offered_size);
        };
#endif

        // if there was a configured size, offer that
        if (ctx->pd_filetransfer_payload > 0)
        {
          offered_size = ctx->pd_filetransfer_payload;
fprintf(stderr, "DEBUG: trimmed offered size to  pd_filetransfer_payload (%d.)\n", offered_size);
        };
        if (ctx->verbosity > 3)
          fprintf(ctx->log, "osdp_FTSTAT FTMsgUpdateMax will be %d.\n", offered_size);

        osdp_doubleByte_to_array(offered_size, response.FtUpdateMsgMax);

        fprintf(ctx->log, " Sending FTSTAT:Offset %d Total %d CurrentSDU %d OfferedSDU %d\n",
          ctx->xferctx.current_offset, ctx->xferctx.total_length, ctx->xferctx.current_send_length,
          offered_size);

        if (ctx->verbosity > 3)
        {
          fprintf(stderr, "current_offset : \"%d\n", ctx->xferctx.current_offset);
          fprintf(stderr, "total_length : %d\n", ctx->xferctx.total_length);
          fprintf(stderr, "current_send_length : %d\n", ctx->xferctx.current_send_length);
          fprintf(stderr, "response mmax %02x %02x\n",
            response.FtUpdateMsgMax [0], response.FtUpdateMsgMax [1]);
        };
        osdp_doubleByte_to_array(OSDP_FTSTAT_OK, response.FtStatusDetail);
        status = oo_send_ftstat(ctx, &response);
      };
    };
  };
  if (status != ST_OK)
  {
    // something bad happened.  abort.  But tell the caller we dealt with it.

    osdp_doubleByte_to_array(OSDP_FTSTAT_ABORT_TRANSFER,
      response.FtStatusDetail);
    status = oo_send_ftstat(ctx, &response);
    if (status EQUALS ST_OK)
      osdp_wrapup_filetransfer(ctx);

    status = ST_OK; // 'cause we recovered.
  };

  // update status json
  if (status EQUALS ST_OK)
    status = oo_write_status (ctx);
  return (status);

} /* action_osdp_FILETRANSFER */


/*
  action_osdp_FTSTAT - processing incoming osdp_FTSTAT message at ACU

  this causes the next chunk to be transferred, or terminates the transfer,
  or switches to "finishing" mode if the PD needs more time.
*/
int
  action_osdp_FTSTAT
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_FTSTAT */

  OSDP_HDR_FTSTAT *ftstat_message;
  int status;


  status = ST_OK;
  osdp_test_set_status(OOC_SYMBOL_cmd_filetransfer, OCONFORM_EXERCISED);
  osdp_test_set_status(OOC_SYMBOL_resp_ftstat, OCONFORM_EXERCISED);
  ftstat_message = (OSDP_HDR_FTSTAT *)(msg->data_payload);

  status = osdp_ftstat_validate(ctx, ftstat_message);
  fprintf(ctx->log, "%s\n", tlogmsg); fflush(ctx->log);
  if (status EQUALS ST_OSDP_FILEXFER_FINISHING)
  {
    // the filetransfer context was already set to "finishing".
    // (and this is ok so reset the status)
    status = ST_OK;
  };
  if (status EQUALS ST_OSDP_FILEXFER_WRAPUP)
  {
    osdp_wrapup_filetransfer(ctx);
    status = ST_OK;
  }
  else
  {
    if (status EQUALS ST_OK)
  {
    // if more send more

    if (ctx->verbosity > 9)
      fprintf(stderr, "t=%d o=%d\n", ctx->xferctx.total_length, ctx->xferctx.current_offset);

    if ((ctx->xferctx.total_length > 0) && (ctx->xferctx.total_length > ctx->xferctx.current_offset))
    {
      status = osdp_send_filetransfer(ctx);
    };

    if ((ctx->xferctx.total_length EQUALS 0) || (ctx->xferctx.total_length EQUALS ctx->xferctx.current_offset))
    {
      fflush(ctx->log);
      osdp_wrapup_filetransfer(ctx);
// make sure removing this doesn't break wavelynx...      status = oo_send_filetransfer(ctx); // will send benign msg
    };
  };
  };
  if (status EQUALS ST_OK)
    status = oo_write_status (ctx);
  return (status);

} /* action_osdp_FTSTAT */


int
  action_osdp_MFG
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_MFG */

  char cmd [3072]; //C_STRING_MX];
  int count;
  int current_length;
  OSDP_MFG_COMMAND *mfg;
  OSDP_HDR *oh;
  int status;
  int unknown;


  status = ST_OK;
  unknown = 1;
  oh = (OSDP_HDR *)(msg->ptr);
  count = oh->len_lsb + (oh->len_msb << 8);
  count = count - 6; // assumes no SCS header
  if (ctx->secure_channel_use [OO_SCU_ENAB] EQUALS OO_SCS_OPERATIONAL)
    count = count - 2; // for SCS 18
  count = count - msg->check_size;

  mfg = (OSDP_MFG_COMMAND *)(msg->data_payload);

  if (0 EQUALS memcmp(mfg->vendor_code, OOSDP_MFG_VENDOR_CODE, sizeof(OOSDP_MFG_VENDOR_CODE)))
  {
    switch (mfg->mfg_command_id)
    {
    case OOSDP_MFG_PING:
      {
        unsigned char mfg_response [sizeof(struct osdp_mfg_command) + 4];
        OSDP_MFG_COMMAND *mh;

        unknown = 0; // we known this guy
        mh = (OSDP_MFG_COMMAND *)mfg_response;
        memcpy(mh->vendor_code, OOSDP_MFG_VENDOR_CODE, sizeof(OOSDP_MFG_VENDOR_CODE));
        mh->mfg_command_id = OOSDP_MFGR_PING_ACK;
        memcpy(&(mh->data), (char *)&(mfg->data), 4); // arbitrarily copy the 4 detail bytes back at ya
        current_length = 0;
        status = send_message(ctx, OSDP_MFGREP, p_card.addr, &current_length, sizeof(mfg_response), mfg_response);
      };
      break;

    case OOSDP_MFG_PIRATE:
      {
        unsigned char mfg_response [500];
        int response_payload_length;

        unknown = 0; // we known this guy
        memset(mfg_response, 0, sizeof(mfg_response));
        response_payload_length = count - 4; // payload length includes OUI, command
        memcpy(mfg_response, &(mfg->data), msg->data_length);
        current_length = 0;
if (ctx->verbosity > 3)
{
  fprintf(ctx->log, "DEBUG: Authenticate like a pirate - unknown %d response payload length %d first payload octet 0x%02X\n",
    unknown, response_payload_length, mfg->data);
};
        status = send_message_ex (ctx, OSDP_MFGERRR, ctx->pd_address,
          &current_length, response_payload_length, mfg_response,
          OSDP_SEC_SCS_18, 0, NULL);
      };
      break;
    };

    // and after we do whatever we did, call the action script.
    /*
      ACTION SCRIPT ARGS: 1=6 hexit OUI 2=MFG command 2 hexit 3=first octet of data 2hexit 4=data payload length
    */

    sprintf(cmd, "%s//run/ACU-actions/osdp_MFG %02X%02X%02X %02X %02X %d", ctx->service_root,
      mfg->vendor_code [0], mfg->vendor_code [1], mfg->vendor_code [2], mfg->mfg_command_id, mfg->data, msg->data_length);
    system(cmd);
  };
  if (unknown)
  {
    // dunno this mfg command, nak it

    int nak_length;
    unsigned char osdp_nak_response_data [1024];

    current_length = 0;
    osdp_nak_response_data [0] = OO_NAK_UNK_CMD;
    memcpy(osdp_nak_response_data, mfg->vendor_code, 3);
    nak_length = 3;
fprintf(ctx->log, "DEBUG: 5 NAK: %d.\n", osdp_nak_response_data [0]);
    status = send_message (ctx, OSDP_NAK, p_card.addr, &current_length, nak_length,
      osdp_nak_response_data); ctx->sent_naks ++;
  };

  return (status);

} /* action_osdp_MFG */


int
  action_osdp_MFGERRR
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_MFGERRR */

  char cmd [2*8192];
  int count;
  FILE *f;
  int i;
  OSDP_HDR *oh;
  char payload [8192];
  int status;
  char tmp1 [8192];


  status = ST_OK;
  fprintf(ctx->log, "MFGERRR received\n");

  // calculate length of payload

  oh = (OSDP_HDR *)(msg->ptr);
  count = oh->len_lsb + (oh->len_msb << 8);
  count = count - 6; // assumes no SCS header
  if (ctx->secure_channel_use [OO_SCU_ENAB] EQUALS OO_SCS_OPERATIONAL)
    count = count - 2; // for SCS 18
  count = count - msg->check_size;

  // the callout creates osdp-mfg-error.json.  It includes the (current known) OUI
  // for convienience, that was not in the message.
  // arg 1 is the PD address
  // arg 2 is the OUI
  // arg 3 the hex string version of the payload
  // infer arg 3's length from strlen(arg 3)/2

  payload [0] = 0;
  for (i=0; i<count; i++)
  {
    sprintf(tmp1, "%02x", msg->data_payload [i]);
    strcat(payload, tmp1);
  };
  sprintf(cmd, "\"{\\\"1\\\":\\\"%02X\\\",\\\"2\\\":\\\"%02X%02X%02X\\\",\\\"3\\\":\\\"%s\\\"}\"",
    ctx->pd_address,
    ctx->vendor_code [0], ctx->vendor_code [1], ctx->vendor_code [2], payload);
  {
    f = fopen("/opt/osdp-conformance/run/ACU/osdp-mfg-error.json", "w");
    if (f != NULL)\
    {
      fprintf(f, "%s\n", cmd);
      fclose(f);
    };
  };
//  sprintf(cmd, "%02X %d.", *(msg->data_payload), msg->data_length);
  status = oosdp_callout(ctx, "osdp_MFGERRR", cmd);

  osdp_test_set_status(OOC_SYMBOL_resp_mfgerrr, OCONFORM_EXERCISED);
  return(status);

} /* action_osdp_MFGERRR */


int
  action_osdp_PDCAP
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_PDCAP */

  char aux [4096];
  FILE *capf;
  OSDP_PDCAP_ENTRY *entry;
  int i;
  int max_multipart;
  int num_entries;
  unsigned char *ptr;
  char results_filename [3072];
  int status;
  char temp_string [1024];


  status = ST_OK;
  strcpy(aux, "\"capabilities\":[");
  if (ctx->verbosity > 3)
  {
    fprintf(ctx->log,
"action_osdp_PDCAP: message data length is %d.\n", msg->data_length);
    fflush(ctx->log);
  };
  num_entries = msg->data_length / 3;
  ptr = msg->data_payload;
  entry = (OSDP_PDCAP_ENTRY *)ptr;
  for (i=0; i<num_entries; i++)
  {
    // create results json files

    sprintf(results_filename, "%s/results/070-05-%02d-results.json", ctx->service_root, 1+entry->function_code);
    capf = fopen(results_filename, "w");
    fprintf(capf, "{\"test\":\"070-05-%02d\",\"test-status\":\"1\",\"pdcap-function\":\"%d\",\"pdcap-compliance\":\"%d\",\"pdcap-number\":\"%d\"}\n",
      entry->function_code+1, entry->function_code, entry->compliance, entry->number_of);
    fclose(capf);

    sprintf(temp_string, "{\"function\":\"%02x\",\"compliance\":\"%02x\",\"number-of\":\"%02x\"},",
      entry->function_code, entry->compliance, entry->number_of);
    if (ctx->verbosity > 3)
    {
      fprintf(ctx->log, "f %02x c %02x n %02x old len %d.\n",
        entry->function_code, entry->compliance, entry->number_of, (int)strlen(aux));
      fflush(ctx->log);
    };
    strcat(aux, temp_string);

    switch (entry->function_code)
    {
    case OSDP_CAP_AUDIBLE_OUT:
      fprintf(ctx->log, "PD: Annunciator present. ");

      // here in the ACU record the PD says it has a buzzer
      ctx->configured_sounder = 1; // only one per spec

      switch(entry->compliance)
      {
      case 1:
        fprintf(ctx->log, "On/Off only.\n");
        break;
      case 2:
        fprintf(ctx->log, "Timed and On/Off.\n");
        break;
      default:
        fprintf(ctx->log, "Not defined (%02x %02x)\n",
          entry->compliance, entry->number_of);
        break;
      };
      break;
    case OSDP_CAP_BIOMETRICS:
      ctx->configured_biometrics = 1;
      break;
    case OSDP_CAP_CARD_FORMAT:
      switch(entry->compliance)
      {
      case 1:
        fprintf(ctx->log, "Card format: Binary.\n");
        break;
      case 2:
        fprintf(ctx->log, "Card format: BCD.\n");
        break;
      case 3:
        fprintf(ctx->log, "Card format: Binary or BCD.\n");
        break;
      default:
        fprintf(ctx->log, "Card format: not defined (%02x %02x)\n",
          entry->compliance, entry->number_of);
        break;
      };
      break;
    case OSDP_CAP_CHECK_CRC:
      if ((entry->compliance EQUALS 0) && (m_check EQUALS OSDP_CRC))
      {
        fprintf(ctx->log,
"WARNING: Device does not support CRC but CRC configured.\n");
      };
      break;
    case OSDP_CAP_CONTACT_STATUS:
      fprintf(ctx->log, "Capability not processed in this ACU: Contact Status (%d)\n",
        entry->function_code);
      break;
    case OSDP_CAP_LED_CONTROL:
      if (ctx->verbosity > 9)
        fprintf(ctx->log, "Capability not processed in this ACU: LED Control (%d)\n",
          entry->function_code);
      break;
    case OSDP_CAP_MAX_MULTIPART:
      max_multipart = entry->compliance;
      max_multipart = max_multipart + (256*entry->number_of);
      fprintf(ctx->log, "PD: largest combined message %d.(0x%x)\n",
        max_multipart, max_multipart);
      break;
    case OSDP_CAP_OUTPUT_CONTROL:
      fprintf(ctx->log, "Capability not processed in this ACU: Output Control (%d)\n",
        entry->function_code);
      break;
    case OSDP_CAP_READERS:
      fprintf(ctx->log, "PD: %d. Attached readers (%x)\n", entry->number_of, entry->compliance);
      break;
    case OSDP_CAP_REC_MAX:
      ctx->pd_cap.rec_max = entry->compliance + 256*entry->number_of;
      break;
    case OSDP_CAP_SECURE:
      if (entry->compliance EQUALS 0)
      {
        if (ctx->enable_secure_channel > 0)
          fprintf(ctx->log, "Secure Channel not supported by PD, disabling (was enabled.)\n");
        ctx->enable_secure_channel = 0;
      };
      break;
    case OSDP_CAP_SMART_CARD:
      if (entry->compliance & 1)
      {
        ctx->pd_cap.smart_card_transparent = 1;
        fprintf(ctx->log, "PD Supports Transparent Mode\n");
      };
      if (entry->compliance & 2)
      {
        ctx->pd_cap.smart_card_extended_packet_mode = 1;
        fprintf(ctx->log, "PD Supports Extended Packet Mode\n");
      };
      break;
    case OSDP_CAP_SPE:
      fprintf(ctx->log, "Capability not processed in this ACU: Secure PIN Entry\n");
      break;
    case OSDP_CAP_TEXT_OUT:
      fprintf(ctx->log, "Capability not processed in this ACU: Text Output (%d)\n",
        entry->function_code);
      break;
    case OSDP_CAP_TIME_KEEPING:
      fprintf(ctx->log, "Capability not processed in this ACU: Time Keeping (%d)\n",
        entry->function_code);
      break;
    case OSDP_CAP_VERSION:
      fprintf(ctx->log, "PD supports OSDP version %d\n", entry->compliance);
      break;
    default:
      status = ST_OSDP_UNKNOWN_CAPABILITY;
      fprintf(ctx->log, "unknown capability: 0x%02x\n", entry->function_code);
      status = ST_OK;
      break;
    };
    entry ++;
  };
  fprintf(ctx->log, "PD Capabilities response processing complete.\n\n");
  if (ctx->last_command_sent EQUALS OSDP_CAP)
    osdp_test_set_status(OOC_SYMBOL_cmd_cap, OCONFORM_EXERCISED);
  strcat(aux, "{\"function\":\"0\",\"compliance\":\"0\",\"number-of\":\"0\"}],");

  osdp_test_set_status_ex(OOC_SYMBOL_rep_device_capas, OCONFORM_EXERCISED, aux);
  return(status);

} /* action_osdp_PDCAP */


int
  action_osdp_POLL
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_POLL */

  int current_length;
  int done;
  unsigned char osdp_lstat_response_data [2];
  unsigned char osdp_raw_data [4+1024];
  int raw_lth;
  int status;


  status = ST_OK;
  done = 0;

  // i.e. we GOT a poll
  osdp_test_set_status(OOC_SYMBOL_cmd_poll, OCONFORM_EXERCISED);

  /*
    poll response can be many things.  we do one and then return, which
    can cause some turn-the-crank artifacts.  may need multiple polls for
    expected behaviors to happen.
  */
  if (!done)
  {
    if (pending_response_length > 0)
    {
      done = 1;
      current_length = 0;
      status = send_message_ex (ctx,
        pending_response, p_card.addr, &current_length,
        pending_response_length, pending_response_data,
        OSDP_SEC_NOT_SCS, 0, NULL);
      pending_response_length = 0;
    };
  };

  // return BUSY if requested

  if (!done)
  {
    if (ctx->next_response EQUALS OSDP_BUSY)
    {
      ctx->next_response = 0;
      done = 1;
      current_length = 0;
      status = send_message_ex (ctx,
        OSDP_BUSY, p_card.addr, &current_length,
        0, NULL, OSDP_SEC_NOT_SCS, 0, NULL);
      SET_PASS (ctx, "4-16-1");
      if (ctx->verbosity > 2)
      {
        sprintf (tlogmsg, "Responding with osdp_BUSY");
        fprintf (ctx->log, "%s\n", tlogmsg);
      };
    };
  };

  // if there was an input status requested return that.

  if ((!done) && (ctx->next_istatr EQUALS 1))
  {
    int input_length;
    unsigned char osdp_istat_response_data [OOSDP_DEFAULT_INPUTS];

    // hard code to show first input active, all others inactive

    memset (osdp_istat_response_data, 0, sizeof (osdp_istat_response_data));
    osdp_istat_response_data [0] = 1; // input 0 is active
    input_length = ctx->configured_inputs;
    osdp_test_set_status(OOC_SYMBOL_resp_istatr, OCONFORM_EXERCISED);

    current_length = 0;
    status = send_message_ex(ctx, OSDP_ISTATR, p_card.addr,
      &current_length, input_length, osdp_istat_response_data, OSDP_SEC_SCS_18, 0, NULL);
    ctx->next_istatr = 0;
    done = 1;
  };

  if ((!done) && (ctx->next_huge EQUALS 1))
  {
    // if a large response test was requested send that
    unsigned char value [2048];

    memset (value, 0, sizeof(value));
    current_length = 0;
    status = send_message_ex(ctx, OSDP_ISTATR, p_card.addr, &current_length, 1300, value, OSDP_SEC_SCS_17, 0, NULL);
    done = 1;
  };

  // if there was a power report or tamper return that.

  if ((!done) && ((ctx->power_report EQUALS 1) || (ctx->tamper)))
  {
    char details [1024];
    done = 1;

    details [0] = 0;
    if (ctx->tamper)
    {
      strcat(details, "Tamper");
      osdp_test_set_status(OOC_SYMBOL_resp_lstatr_tamper, OCONFORM_EXERCISED);

      osdp_test_set_status(OOC_SYMBOL_poll_lstatr, OCONFORM_EXERCISED);
    };
    if (ctx->power_report)
    {
      if (strlen(details) > 0)
        strcat(details, " ");
      strcat(details, "Power");
      osdp_test_set_status(OOC_SYMBOL_resp_lstatr_power, OCONFORM_EXERCISED);

      // and that's an lstatr response to a poll, too.

      osdp_test_set_status(OOC_SYMBOL_poll_lstatr, OCONFORM_EXERCISED);
    };
    osdp_lstat_response_data [ 0] = ctx->tamper;
    osdp_lstat_response_data [ 1] = ctx->power_report;

    // clear tamper and power now reported
    ctx->tamper = 0;
    ctx->power_report = 0;

    current_length = 0;
    status = send_message_ex (ctx,
      OSDP_LSTATR, p_card.addr, &current_length,
      sizeof (osdp_lstat_response_data), osdp_lstat_response_data,
      OSDP_SEC_NOT_SCS, 0, NULL);
    if (ctx->verbosity > 2)
    {
      sprintf (tlogmsg, "Responding with OSDP_LSTATR (%s)", details);
      fprintf (ctx->log, "%s\n", tlogmsg);
    };
  }

  // send an on-demand LSTATR (to clear tamper)

  if (!done)
  {
    if (ctx->next_response EQUALS OSDP_LSTATR)
    {
      ctx->next_response = 0;
      done = 1;
      osdp_lstat_response_data [ 0] = ctx->tamper;
      osdp_lstat_response_data [ 1] = ctx->power_report;

      current_length = 0;
      status = send_message_ex (ctx, OSDP_LSTATR, p_card.addr, &current_length,
        sizeof (osdp_lstat_response_data), osdp_lstat_response_data, OSDP_SEC_NOT_SCS, 0, NULL);
      if (ctx->verbosity > 2)
      {
        sprintf (tlogmsg, "Responding with on-demand osdp_LSTATR (T=%d P=%d)", ctx->tamper, ctx->power_report);
        fprintf (ctx->log, "%s\n", tlogmsg);
      };
    };
  };

  // if there's card data to return, do that.

  if (!done)
  {
    /*
      the presence of card data to return is indicated because either the
      "raw" buffer or the "big" buffer is marked as non-empty when you get here.
    */
    if (ctx->card_data_valid > 0)
    {
      done = 1;
      // send data if it's there (value is number of bits)

      // osdp_RAW is reader, format, countHigh, countLow, data

      osdp_raw_data [ 0] = 0; // one reader, reader 0
      osdp_raw_data [ 1] = ctx->card_format; 
      osdp_raw_data [ 2] = (0xff & ctx->card_data_valid);
      osdp_raw_data [ 3] = (0xff00 & ctx->card_data_valid) >> 8;
      raw_lth = 4+ctx->creds_a_avail;
      memcpy (osdp_raw_data+4, ctx->credentials_data, ctx->creds_a_avail);
      current_length = 0;

      dump_buffer_log(ctx, "card data", (unsigned char *)(ctx->credentials_data), ctx->creds_a_avail);
      status = send_message_ex (ctx,
        OSDP_RAW, p_card.addr, &current_length, raw_lth, osdp_raw_data,
        OSDP_SEC_SCS_18, 0, NULL);
      osdp_test_set_status(OOC_SYMBOL_rep_raw, OCONFORM_EXERCISED);
      if (ctx->verbosity > 2)
      {
        sprintf (tlogmsg, "Responding with cardholder data (%d bits)",
          ctx->card_data_valid);
        fprintf (ctx->log, "%s\n", tlogmsg);
      };
      ctx->card_data_valid = 0;
    }
    else
    {

// DISABLED not working
#if 0
      /*
        this is the newer multi-part message for bigger credential responses,
        like a FICAM CHUID.
      */
      if (ctx->creds_a_avail > 0)
      {
        done = 1;

        // send another mfgrep message back and update things.

        memset (&mmsg, 0, sizeof (mmsg));
        if (creds_buffer_a_lth > 0xfffe)
        {
          status = ST_BAD_MULTIPART_BUF;
        }
        else
        {
          mmsg.VendorCode [0] = 0x08;
          mmsg.VendorCode [1] = 0x00;
          mmsg.VendorCode [2] = 0x1b;
          mmsg.Reply_ID = MFGREP_OOSDP_CAKCert;
          mmsg.MpdSizeTotal = creds_buffer_a_lth;
          mmsg.MpdOffset = creds_buffer_a_next;

          bufsize = sizeof (mmsg); // used in send operation below
          if (ctx->creds_a_avail > 128)
          {
            to_send = 128;
            ctx->creds_a_avail = ctx->creds_a_avail - 128;
          }
          else
          {
            to_send = ctx->creds_a_avail;
            ctx->creds_a_avail = 0;
          };
          mmsg.MpdFragmentSize = to_send; 

          // filled in all of mmsg now copy it to buffer

          memcpy (buffer, &mmsg, sizeof (mmsg));

          // actual data goes after header in buffer.
          memcpy (buffer+bufsize, creds_buffer_a+creds_buffer_a_next, to_send);

          current_length = 0;
          status = send_message (ctx, OSDP_MFGREP, p_card.addr,
            &current_length, bufsize+to_send, buffer);

          // and after all that move the pointer within the buffer for where
          // the next data is extracted from.

          creds_buffer_a_next = creds_buffer_a_next + to_send;
        };
      };
#endif
    };
  };
  /*
    if all else isn't interesting return a plain ack
  */
  if (!done)
  {
    current_length = 0;
    status = send_message_ex
      (ctx, OSDP_ACK, p_card.addr, &current_length, 0, NULL,
      OSDP_SEC_SCS_16, 0, NULL);
    osdp_test_set_status(OOC_SYMBOL_cmd_poll, OCONFORM_EXERCISED);
    osdp_test_set_status(OOC_SYMBOL_rep_ack, OCONFORM_EXERCISED);
    if (ctx->verbosity > 9)
    {
      sprintf (tlogmsg, "Responding with OSDP_ACK");
      fprintf (ctx->log, "%s\n", tlogmsg);
    };
  };

  // update status json.  perhaps every single poll is too often.
  // TODO add another timer, do this perhaps once a second.
  // for now kludge it and only do it if in verbose mode

  if (status EQUALS ST_OK)
    if (ctx->verbosity > 3)
      status = oo_write_status (ctx);

  return (status);

} /* action_osdp_POLL */


int
  action_osdp_RAW
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_RAW */

  int bits;
  char cmd [16384]; // bigger than hex_details
  OSDP_COMMAND command_for_later;
  char details [1024];
  int display;
  char hex_details [4096];
  char hstr [1024]; // hex string of raw card data payload
  unsigned char *raw_data;
  int status;


  status = ST_OK;
  display = 0; // assume encrypted and can't see it.
  details [0] = 0;

  if (ctx->role EQUALS OSDP_ROLE_CP) // check this, should not need to check this here.
  {
    (void)oosdp_make_message (OOSDP_MSG_RAW, tlogmsg, msg);
    fprintf(ctx->log, "%s\n", tlogmsg); fflush(ctx->log); tlogmsg [0] = 0;

    raw_data = msg->data_payload + 4;
    dump_buffer_log(ctx, "osdp_RAW data", msg->data_payload, msg->data_length);
    if (msg->security_block_length > 0)
    {
      fprintf(ctx->log, "  RAW card data contents encrypted.\n");
    };
    if (msg->payload_decrypted)
    {
      fprintf(ctx->log, "  Contents was decrypted.\n");
      display = 1;
    };
    if (msg->security_block_length EQUALS 0)
      display = 1;
    if (display)
    {
      char raw_fmt [1024];
      /*
        this processes an osdp_RAW.  byte 0=rdr, b1=fmt, 2-3 are length (2=lsb)
      */

      strcpy(raw_fmt, "unspecified");
      if (*(msg->data_payload+1) EQUALS 1)
        strcpy(raw_fmt, "P/data/P");
      if (*(msg->data_payload+1) > 1)
        sprintf(raw_fmt, "unknown(%d)", *(msg->data_payload+1));

      fprintf(ctx->log, "Raw data: Format %s (Reader %d) ", raw_fmt, *(msg->data_payload+0));
      bits = *(msg->data_payload+2) + ((*(msg->data_payload+3))<<8);
      ctx->last_raw_read_bits = bits;

      fprintf(ctx->log, "%d. bits: ", bits);
      {
        int i;
        int octets;
        char tstr [32];

        octets = (bits+7)/8;
        if (octets > sizeof (ctx->last_raw_read_data))
          octets = sizeof (ctx->last_raw_read_data);

        hstr[0] = 0;
        for (i=0; i<octets; i++)
        {
          sprintf (tstr, "%02x", *(raw_data+i));
          strcat (hstr, tstr);
        };
        fprintf(ctx->log, "%s\n", hstr);

        memcpy (ctx->last_raw_read_data, raw_data, octets);
      };

      status = oo_write_status (ctx);

      /*
        build up raw details for the results file.
        (yeah I decode it several times.)
      */
      {
        int i;
        char octet [3];

        hex_details [0] = 0;
        strcpy(details, "\"payload\":\"");
        for (i=0; i<msg->data_length; i++)
        {
          sprintf(octet, "%02x", *(msg->data_payload+i));
          strcat(details, octet);
          strcat(hex_details, octet);
        };
        strcat(details, "\",");
      };

      // run the action routine with the bytes,bit count,format, details

      sprintf(cmd, "%s/osdp_RAW %s %d %d %s",
        oo_osdp_root(ctx, OO_DIR_ACTIONS),
        hstr, bits, *(msg->data_payload+1), hex_details);
      system(cmd);
    }; // not encrypted

    // I'm the ACU, I got an osdp_RAW, report results and details

    osdp_test_set_status_ex(OOC_SYMBOL_rep_raw, OCONFORM_EXERCISED, details);
  };

  switch(ctx->raw_reaction_command)
  {
  case 0x00:
    // do nothing special
    break;

  default:
    fprintf(ctx->log, "unknown raw_reaction_command %02X\n", ctx->raw_reaction_command);
    ctx->raw_reaction_command = 0;
    break;

  case OSDP_CRAUTH:
    {
      fprintf(ctx->log, "Exercising test 060-25-03");
    };
    break;

  case OSDP_LED:
    // immediately send a LED response (blinking Red perhaps?)
fprintf(stderr, "DEBUG: insert LED red blink here.\n");

    memset(&command_for_later, 0, sizeof(command_for_later));
    command_for_later.command = OSDP_CMDB_LED;
    memcpy(command_for_later.details, ctx->test_details, ctx->test_details_length);
    command_for_later.details_length = ctx->test_details_length;
    ctx->raw_reaction_command = 0;
    status = enqueue_command(ctx, &command_for_later);
    break;

  case OSDP_GENAUTH:
    {
      fprintf(ctx->log, "Exercising test 060-24-03");
    };
    break;
  };

  // if a (react to raw) genauth was requested, enqueue the request

  if (0 EQUALS strcmp (ctx->test_in_progress, "060-24-03"))
  {
    // stick in a poll so the next command is not back to back with the osdp_RAW response

    memset(&command_for_later, 0, sizeof(command_for_later));
    command_for_later.command = OSDP_CMDB_SEND_POLL;
    status = enqueue_command(ctx, &command_for_later);
    memset(&command_for_later, 0, sizeof(command_for_later));
    command_for_later.command = OSDP_CMDB_WITNESS;
    memcpy(command_for_later.details, ctx->test_details, ctx->test_details_length);
    command_for_later.details_length = ctx->test_details_length;
    ctx->test_details_length = 0; // done with it, "clear" the buffer.

    status = enqueue_command(ctx, &command_for_later);

    // say they did this command.  report generator will know this vs. a crauth

    osdp_test_set_status(OOC_SYMBOL_060_24_03, OCONFORM_EXERCISED);
  };

  // if a (react to raw) crauth was requested, enqueue the request

  if (0 EQUALS strcmp (ctx->test_in_progress, "060-25-03"))
  {
fprintf(stderr, "DEBUG: inserted POLL before CRAUTH\n");
    memset(&command_for_later, 0, sizeof(command_for_later));
    command_for_later.command = OSDP_CMDB_SEND_POLL;
    status = enqueue_command(ctx, &command_for_later);
fprintf(stderr, "DEBUG: crauth enqueued\n");
    memset(&command_for_later, 0, sizeof(command_for_later));
    command_for_later.command = OSDP_CMDB_CHALLENGE;
    memcpy(command_for_later.details, ctx->test_details, ctx->test_details_length);
    command_for_later.details_length = ctx->test_details_length;
    ctx->test_details_length = 0; // done with it, "clear" the buffer.

    status = enqueue_command(ctx, &command_for_later);

    // say they did this command.  report generator will know this vs. a crauth

    osdp_test_set_status(OOC_SYMBOL_060_25_03, OCONFORM_EXERCISED);
  };

  // if a genauth-after-raw was requested, do it now.

  if (0 EQUALS strcmp (ctx->test_in_progress, "060-24-02"))
  {
    int current_length;
    unsigned char details [OSDP_OFFICIAL_MSG_MAX];
    int details_length;
    unsigned char payload [OSDP_OFFICIAL_MSG_MAX];
    int payload_length;


    memset(details, 0, sizeof(details));
    details_length = 270; // estimated null payload ... //sizeof(details);
    if (ctx->test_details_length > 0)
    {
      if (ctx->test_details_length < OSDP_OFFICIAL_MSG_MAX)
      {
        memcpy(details, ctx->test_details, ctx->test_details_length);
        details_length = ctx->test_details_length;
        ctx->test_details_length = 0;  // it's been consumed.
      };
    };
    payload_length = sizeof(payload);
    status = oo_build_genauth(ctx, payload, &payload_length, details, details_length);
    if (status EQUALS ST_OK)
    {
      current_length = 0;
      status = send_message_ex
        (ctx, OSDP_GENAUTH, p_card.addr, &current_length, payload_length, payload,
        OSDP_SEC_SCS_17, 0, NULL);
fprintf(stderr, "DEBUG: give GENAUTH a chance...\n"); sleep(5);
    };
  };

  // if a crauth-after-raw was requested, do it now.

  if (0 EQUALS strcmp (ctx->test_in_progress, "060-25-02"))
  {
    int current_length;
    unsigned char details [OSDP_OFFICIAL_MSG_MAX];
    int details_length;
    unsigned char payload [OSDP_OFFICIAL_MSG_MAX];
    int payload_length;

    memset(details, 0, sizeof(details));
    details_length = 270; // estimated null payload ... //sizeof(details);
    if (ctx->test_details_length > 0)
    {
      if (ctx->test_details_length < OSDP_OFFICIAL_MSG_MAX)
      {
        memcpy(details, ctx->test_details, ctx->test_details_length);
        details_length = ctx->test_details_length;
        ctx->test_details_length = 0;  // it's been consumed.
      };
    };

    // build up the payload.  The SDU looks the same for genauth and crauth so
    // we can use the same routine and just send a different command.

    payload_length = sizeof(payload);
    status = oo_build_genauth(ctx, payload, &payload_length, details, details_length);
    if (status EQUALS ST_OK)
    {
      current_length = 0;
      status = send_message_ex
        (ctx, OSDP_CRAUTH, p_card.addr, &current_length, payload_length, payload,
        OSDP_SEC_SCS_17, 0, NULL);
fprintf(stderr, "DEBUG: give CRAUTH a chance...\n"); sleep(5);
    };
  };

  return (status);

} /* action_osdp_RAW */


int
  action_osdp_TEXT
    (OSDP_CONTEXT *ctx,
    OSDP_MSG *msg)

{ /* action_osdp_TEXT */

  int current_length;
  int status;
  int text_length;
  char tlogmsg  [1024];


  status = ST_OK;
  osdp_test_set_status(OOC_SYMBOL_cmd_text, OCONFORM_EXERCISED);

  memset (ctx->text, 0, sizeof (ctx->text));
  text_length = (unsigned char) *(msg->data_payload+5);
  strncpy (ctx->text, (char *)(msg->data_payload+6), text_length);

  fprintf (ctx->log, "Text:");
  fprintf (ctx->log,
    " Rdr %x tc %x tsec %x Row %x Col %x Lth %x\n",
     *(msg->data_payload + 0), *(msg->data_payload + 1), *(msg->data_payload + 2),
      *(msg->data_payload + 3), *(msg->data_payload + 4), *(msg->data_payload + 5));

  memset (tlogmsg, 0, sizeof (tlogmsg));
  strncpy (tlogmsg, (char *)(msg->data_payload+6), text_length);
  fprintf (ctx->log, "Text: %s\n", tlogmsg);

  // we always ack the TEXT command regardless of param errors

  current_length = 0;
//  status = send_message (ctx, OSDP_ACK, p_card.addr, &current_length, 0, NULL);
  status = send_message_ex (ctx, OSDP_ACK, p_card.addr, &current_length, 0, NULL, OSDP_SEC_SCS_16, 0, NULL);
  ctx->pd_acks ++;
  if (ctx->verbosity > 2)
    fprintf (ctx->log, "Responding with OSDP_ACK\n");

  return (status);

} /* action_osdp_TEXT */

