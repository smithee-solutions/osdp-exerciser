/*
  oo-parse - parse osdp messages

  (C)Copyright 2017-2021 Smithee Solutions LLC

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
#include <unistd.h>
#include <time.h>
#include <stdlib.h>


#include <osdp-tls.h>
#include <open-osdp.h>
#include <osdp_conformance.h>
#include <iec-xwrite.h>


extern OSDP_CONTEXT context;
extern OSDP_INTEROP_ASSESSMENT osdp_conformance;
extern OSDP_PARAMETERS p_card;
extern OSDP_BUFFER osdp_buf;
unsigned char last_command_received;
unsigned char last_sequence_received;
unsigned char last_check_value;
extern char trace_in_buffer [];


/*
  osdp_parse_message - parses OSDP message

  Note: if verbosity is set (global m_verbosity) it also prints the PDU
  to stderr.
*/
int
  osdp_parse_message
    (OSDP_CONTEXT *context,
    int role,
    OSDP_MSG *m,
    OSDP_HDR *returned_hdr)

{ /* osdp_parse_message */

  int display;
  int hashable_length;
  int i;
  unsigned int msg_lth;
  int msg_check_type;
  int msg_data_length;
  int msg_scb;
  int msg_sqn;
  OSDP_HDR *p;
  unsigned short int parsed_crc;
  int sec_blk_length;
  int sec_block_type;
  int status;
  unsigned wire_cksum;
  unsigned short int wire_crc;


  p = (OSDP_HDR *)m->ptr;

  status = ST_MSG_TOO_SHORT;

  m->data_payload = NULL;
  m->security_block_length = 0; // assume no security block
  msg_data_length = 0;

  msg_check_type = (p->ctrl) & 0x04;
  if (msg_check_type EQUALS 0)
  {
    m->check_size = 1;
// do NOT change m_check global just because this packet was different...    m_check = OSDP_CHECKSUM; // Issue #11
//    if (context->verbosity > 2) fprintf(context->log, "m_check set to CHECKSUM (parse)\n");
  }
  else
  {
    m->check_size = 2;
//    m_check = OSDP_CRC;
  };

  if (m->lth > OSDP_OFFICIAL_MSG_MAX)
    status = ST_MSG_TOO_LONG;

  if ((status EQUALS ST_OK) || (status EQUALS ST_MSG_TOO_SHORT))
  {
    if (m->lth >= (m->check_size+sizeof (OSDP_HDR)))
    {
      //fprintf(stderr, "DEBUG: (osdp_parse_message) length acceptable\n");
      status = ST_OK;
      msg_lth = p->len_lsb + (256*p->len_msb);
      if (msg_lth > OSDP_OFFICIAL_MSG_MAX)
        status = ST_MSG_TOO_LONG;
      if (status EQUALS ST_OK)
      {
        hashable_length = msg_lth;
        if ((m->lth) > msg_lth)
          m->remainder = msg_lth - m->lth;

        /*
          now that we have a bit of header figure out how much the whole thing is.
          need all of it to process it.
        */
        if (m->lth < msg_lth)
          status = ST_MSG_TOO_SHORT;
      };
    };
  };
  if (status != ST_OK)
  {
    if (status != ST_MSG_TOO_SHORT)
    {
      fprintf (context->log,
        "parse_message did not clear the header.  msg_data_length %d. msg_check_type 0x%x m->check_size %d. m->lth %d. msg_lth %d status %d.\n",
        msg_data_length, msg_check_type, m->check_size, m->lth, msg_lth,
        status);
      fflush (context->log);
    };
  };
  if (status EQUALS ST_OK)
  {    
    tlogmsg [0] = 0;
   
    // must start with SOM
    if (p->som != C_SOM)
      status = ST_MSG_BAD_SOM;
  };
  if (status EQUALS ST_OK)
  {
    // first few fields are always in same place
    returned_hdr -> som = p->som;
    returned_hdr -> addr = 0x7f & p->addr; // low 7 bits are address
    returned_hdr -> len_lsb = p->len_lsb;
    returned_hdr -> len_msb = p->len_msb;
    returned_hdr -> ctrl = p->ctrl;

    // various control info in CTRL byte
    msg_sqn = (p->ctrl) & 0x03;
    m->sequence = msg_sqn;
    msg_scb = (p->ctrl) & 0x08;

    // depending on whether it's got a security block or not
    // the command/data starts at a different place
    if (msg_scb EQUALS 0)
    {
      m -> cmd_payload = m->ptr + 5;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
    }
    else
    {
      tlogmsg[0] = 0;
      sprintf(tlogmsg2, "Msg (Secure): ");
      strcat(tlogmsg, tlogmsg2);
      for (i=0; i<16; i++)
      {
        sprintf(tlogmsg2, "%02x", (m->ptr)[i]);
        strcat(tlogmsg, tlogmsg2);
        if (3 == (i % 4))
          if (i != 15)
          {
            sprintf(tlogmsg2, "-");
            strcat(tlogmsg, tlogmsg2);
          };
      };
      strcat(tlogmsg, "\n");
      tlogmsg [0] = 0;
      tlogmsg2 [0] = 0;

      // packet is SOM, ADDR, LEN_LSB, LEN_MSB, CTRL (5 bytes) and then...

      // sec_blk_length -

      msg_data_length = p->len_lsb + (p->len_msb << 8);

      // if there's a security block and it's the proper type here's 4 bytes
      // of MAC at the end.

      sec_block_type = (unsigned)*(m->ptr+6); // second byte of sec block
      if ((sec_block_type EQUALS OSDP_SEC_SCS_15) ||
        (sec_block_type EQUALS OSDP_SEC_SCS_16) ||
        (sec_block_type EQUALS OSDP_SEC_SCS_17) ||
        (sec_block_type EQUALS OSDP_SEC_SCS_18))
        msg_data_length = msg_data_length - 4;
      sec_blk_length = (unsigned)*(m->ptr + 5);
      m->security_block_type = sec_block_type;
      m->security_block_length = sec_blk_length;
      if (m->security_block_length > 0)
        osdp_test_set_status(OOC_SYMBOL_security_block, OCONFORM_EXERCISED);


      m -> cmd_payload = m->ptr + 5 + sec_blk_length;

      // whole thing less 5 hdr less 1 cmd less sec blk less 2 crc
      msg_data_length = msg_data_length - 6 - sec_blk_length - 2;

      fflush (stdout);fflush (stderr);
    };

    // extract the command
    returned_hdr -> command = (unsigned char) *(m->cmd_payload);
    m->msg_cmd = returned_hdr->command;
    m->direction = 0x80 & p->addr;
    m->data_payload = m->cmd_payload + 1;

    // if it wasn't a poll or an ack report the secure header if there is one
    if ((m->msg_cmd != OSDP_POLL) && (m->msg_cmd != OSDP_ACK))
      fprintf (context->log, "%s", tlogmsg);
    if ((context->verbosity > 2) || (m->msg_cmd != OSDP_ACK))
    {
      sprintf (tlogmsg2, " Cmd %02x", returned_hdr->command);
      strcat (tlogmsg, tlogmsg2);
    };

    // display it.  Unless it's a poll/ack or filetransfer/ftstat.  or verbosity is high enough.

    display = 0;
    if (context->verbosity > 4)
      display = 1;
    if ((m->msg_cmd != OSDP_POLL) && (m->msg_cmd != OSDP_ACK))
    {
      display = 1;

//      if ((m->msg_cmd != OSDP_FILETRANSFER) && (m->msg_cmd != OSDP_FTSTAT))
      {
        display = 1;
      };
      if ((m->msg_cmd EQUALS OSDP_FILETRANSFER) &&
          (context->xferctx.current_offset EQUALS 0))
      {
        display = 1;
      };
      if (m->msg_cmd EQUALS OSDP_FTSTAT)
      {
        unsigned short int ft_status_detail;
        OSDP_HDR_FTSTAT *ft;

        ft = (OSDP_HDR_FTSTAT *)(m->data_payload);
        osdp_array_to_doubleByte(ft->FtStatusDetail, &ft_status_detail);
        if (ft_status_detail != 0)
        {
          display = 1;
        };
      };
    };
if (m->msg_cmd EQUALS OSDP_FILETRANSFER)
  display = 1;

    if (context->verbosity > 9)
    {
      if (p->ctrl & 0x08)
      {
        fprintf(stderr, "DEBUG: SCS\n");
      };
    };

    if (display)
    {
      char dirtag [1024];
      unsigned char *p1;
      char tlogmsg [1024];


        strcpy (tlogmsg, "");
        p1 = m->ptr;
        if (*(p1+1) & 0x80)
          strcpy (dirtag, "PD");
        else
          strcpy (dirtag, "ACU");
        if (0 EQUALS strcmp (dirtag, "ACU"))
          status = oosdp_log (context, OSDP_LOG_STRING_CP, 1, tlogmsg);
        else
          status = oosdp_log (context, OSDP_LOG_STRING_PD, 1, tlogmsg);
      
// don't dump sec block here, gets dumped in oo_util 
        // p2 = p1+5; // command/reply
        if (0) //(p->ctrl & 0x08)
        {
          strcpy(tlogmsg, osdp_sec_block_dump(p1+5));
          fprintf(context->log, "%s\n", tlogmsg);
          fflush (context->log);
          // p2 = p1+5+*(p1+5); // before-secblk and secblk
        };
    };

    m->data_length = msg_data_length;
    // go check the command field
    status = osdp_check_command_reply (role, returned_hdr->command, m, tlogmsg2);
    msg_data_length = m->data_length;

    // if we're the ACU and we are looking at sequence 0 then the DUT passes the seq zero test

    if (1) // status is ST_OK or status is ST_OSDP_CMDREP_FOUND ?
    {

      if ((context->role EQUALS OSDP_ROLE_ACU) && (context->role EQUALS role))
      {
        if (msg_sqn EQUALS 0)
        {
          osdp_test_set_status(OOC_SYMBOL_seq_zero, OCONFORM_EXERCISED);
        };
      };
    };
    if (status != ST_OSDP_CMDREP_FOUND)
    {
      if (status != ST_OK)
        fprintf (context->log,
          "***Status %d Unknown command? (%02x), default msg_data_length was %d\n",
          status, returned_hdr->command, msg_data_length);

    if (context->verbosity > 8)
    {
      fprintf(context->log, "osdp_parse_message: command %02x\n", returned_hdr->command);
    };

fprintf(stderr, "DEBUG: osdp_parse_message cmd %02x\n", returned_hdr->command);
    switch (returned_hdr->command)
    {
    default:
      if ((context->role EQUALS OSDP_ROLE_PD) && !(0x80 & p->addr))
      {
        // it's not for another PD
        m->data_payload = m->cmd_payload + 1;
        msg_data_length = 0;
        if (context->verbosity > 2)
          strcpy (tlogmsg2, "\?\?\?");

        // if we don't recognize the command/reply code it fails 2-15-1
        osdp_test_set_status(OOC_SYMBOL_CMND_REPLY, OCONFORM_FAIL);
      };
      break;

    case OSDP_ACURXSIZE:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_ACURXSIZE");
      osdp_test_set_status(OOC_SYMBOL_cmd_acurxsize, OCONFORM_EXERCISED);
      break;

    case OSDP_BIOMATCH:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_BIOMATCH");
      osdp_test_set_status(OOC_SYMBOL_cmd_biomatch, OCONFORM_EXERCISED);
      break;

    case OSDP_BIOREAD:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_BIOREAD");
      osdp_test_set_status(OOC_SYMBOL_cmd_bioread, OCONFORM_EXERCISED);
      break;

    case OSDP_BUSY:
      m->data_payload = NULL;
      msg_data_length = 0;
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_BUSY");
      osdp_test_set_status(OOC_SYMBOL_resp_busy, OCONFORM_EXERCISED);
      break;

    case OSDP_FTSTAT:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_FTSTAT");
      break;

    case OSDP_NAK:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      context->sent_naks ++;
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_NAK");
      break;

    case OSDP_BUZ:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_BUZ");
      break;

    case OSDP_CAP:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_CAP");
      break;

    case OSDP_COM:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_COM");
      break;

    case OSDP_COMSET:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = 5;
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_COMSET");
      break;

    case OSDP_ID:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_ID");
      break;

    case OSDP_ISTAT:
      m->data_payload = NULL;
      msg_data_length = 0;
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_ISTAT");
      break;

   case OSDP_ISTATR:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_ISTATR");
      break;

    case OSDP_KEEPACTIVE:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_KEEEPACTIVE");
      break;

    case OSDP_KEYPAD:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_KEYPAD");
      break;

    case OSDP_LED:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_LED");
      break;

    case OSDP_LSTAT:
fprintf(stderr, "lstat 1000\n");
      m->data_payload = NULL;
      msg_data_length = 0;
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_LSTAT");
      break;

   case OSDP_LSTATR:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_LSTATR");
      break;

#if 0
    case OSDP_MFG:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_MFG");
      break;
#endif

    case OSDP_MFGERRR:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_MFGERRR");
      break;

    case OSDP_MFGREP:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6;
      msg_data_length = msg_data_length - m->check_size; // 1 for checksum 2 for CRC
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_MFGREP");
      break;

    case OSDP_OSTAT:
      m->data_payload = NULL;
      msg_data_length = 0;
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_OSTAT");
      break;

    case OSDP_OSTATR:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_OSTATR");
      break;

    case OSDP_OUT:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_OUT");
      break;

    case OSDP_PDCAP:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_PDCAP");
      osdp_test_set_status(OOC_SYMBOL_cmd_cap, OCONFORM_EXERCISED);
      osdp_test_set_status(OOC_SYMBOL_rep_device_capas, OCONFORM_EXERCISED);
      break;

    case OSDP_PDID:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk

      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_PDID");

      // if we had sent an osdp_ID then that worked.
      if (context->last_command_sent EQUALS OSDP_ID)
        osdp_test_set_status(OOC_SYMBOL_cmd_id, OCONFORM_EXERCISED);
      break;

    case OSDP_PIVDATA:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_PIVDATA");
      osdp_test_set_status(OOC_SYMBOL_cmd_pivdata, OCONFORM_EXERCISED);
      break;

    case OSDP_PIVDATAR:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_PIVDATAR");
      osdp_test_set_status(OOC_SYMBOL_resp_pivdatar, OCONFORM_EXERCISED);
      break;

    case OSDP_RAW:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_RAW");
      break;

    case OSDP_RSTAT:
      m->data_payload = NULL;
      msg_data_length = 0;
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_RSTAT");
      break;

    case OSDP_RSTATR:
      // sending osdp_RSTATR
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      osdp_test_set_status(OOC_SYMBOL_resp_rstatr, OCONFORM_EXERCISED);
      // if this is in response to an RSTAT then mark that too.
      if (context->last_command_sent EQUALS OSDP_RSTAT)
        osdp_test_set_status(OOC_SYMBOL_cmd_rstat, OCONFORM_EXERCISED);
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_RSTATR");
      break;

    case OSDP_TEXT:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_TEXT");
      break;

    case OSDP_XRD:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_XRD");
      break;

    case OSDP_XWR:
      m->data_payload = m->cmd_payload + 1;
      msg_data_length = p->len_lsb + (p->len_msb << 8);
      msg_data_length = msg_data_length - 6 - 2; // less hdr,cmnd, crc/chk
      if (context->verbosity > 2)
        strcpy (tlogmsg2, "osdp_XWR");
      break;
    };
    }; // bolt-on for PD/CP switch statements.
    // if it was found it's ok
    if (status EQUALS ST_OSDP_CMDREP_FOUND)
      status = ST_OK;

    // for convienience save the data payload length

///    m->data_length = msg_data_length;

    // crc_check is a pointer, used even if it's a checksum

    m->crc_check = m->cmd_payload + 1 + msg_data_length;

    if (m->check_size EQUALS 2)
    {
      hashable_length = hashable_length - 2;

      // figure out where crc or checksum starts
//...

      // if it's an SCS with a MAC suffix move the CRC pointer

      if (msg_scb)
      {

        // if we're the CP and we get an SCS but we're not in Secure Channel then
        // ditch the link session

        if ((sec_block_type EQUALS OSDP_SEC_SCS_15) || (sec_block_type EQUALS OSDP_SEC_SCS_16) ||
          (sec_block_type EQUALS OSDP_SEC_SCS_17) || (sec_block_type EQUALS OSDP_SEC_SCS_18))
        {
          if ((context->secure_channel_use [OO_SCU_ENAB] != OO_SCS_OPERATIONAL) &&
            (context->role EQUALS OSDP_ROLE_ACU))
          {
            fprintf(context->log, "sec_block_type was %x but not in secure channel, resetting\n",
              sec_block_type);
            status = ST_SCS_FROM_PD_UNEXPECTED;
            context->next_sequence = 0;
          }
          else
          {
            m->crc_check = 4 + m->cmd_payload + 1 + msg_data_length;
          };
        };
      };

      parsed_crc = fCrcBlk (m->ptr, msg_lth - 2);

      wire_crc = *(1+m->crc_check) << 8 | *(m->crc_check);
      if (parsed_crc != wire_crc)
      {
        if (context->verbosity > 2)
        {
          fprintf(context->log, "Bad CRC: Got %04x Expected %04x\n",
            wire_crc, parsed_crc);
        };
        status = ST_BAD_CRC;
        context->crc_errs ++;
        if (context->role EQUALS OSDP_ROLE_ACU)
          osdp_test_set_status(OOC_SYMBOL_CRC_bad_response, OCONFORM_EXERCISED);
        else
          osdp_test_set_status(OOC_SYMBOL_CRC_bad_command, OCONFORM_EXERCISED);
      };
      if (status EQUALS ST_OK)
      {
        last_check_value = wire_crc;
        last_command_received = m->msg_cmd;
      };
    }
    else
    {
      unsigned parsed_cksum;

      hashable_length = hashable_length - 1;

      // checksum

      parsed_cksum = checksum (m->ptr, m->lth-1);

      // last byte is checksum

      wire_cksum = (unsigned char)*(m->lth -1 + m->ptr);

      if (context->verbosity > 99)
      {
        fprintf (stderr, "pck %04x wck %04x\n",
          parsed_cksum, wire_cksum);
      };
      if (parsed_cksum != wire_cksum)
      {
char *p;
int i;
        fprintf(context->log, "CHECKSUM ERROR Parsed=0x%02x Wire=0x%02x\n",
          parsed_cksum, wire_cksum);
fprintf(stderr, "Checksum error != c=%x p %x %x\n",
  (unsigned)(returned_hdr->command), (unsigned)parsed_cksum, (unsigned)wire_cksum);
p = (char *)(m->ptr);
for (i=0; i<16; i++)
  fprintf(stderr, " %02x", *(unsigned char *)(p+i)); 
fprintf(stderr, "\n"); fflush(stderr);
        status = ST_BAD_CHECKSUM;
status = ST_OK; // tolerate checksum error and continue
        context->checksum_errs ++;
      };
      if (status EQUALS ST_OK)
      {
        last_check_value = wire_cksum;
        last_command_received = m->msg_cmd;
      };

    };

    // if the sequence number didn't line up report
    {
      int bad;
      int rcv_seq;
      int wire_sequence;

      wire_sequence = msg_sqn;
      rcv_seq = msg_sqn;

      //increment by 1, loops around at 3, never goes back to zero
      rcv_seq = (rcv_seq + 1) % 4;
      if (!rcv_seq)
        rcv_seq = 1;

      if (context->verbosity > 9)
        fprintf(stderr, "DEBUG: wire seq %d. rcv seq %d. next seq %d.\n",
          wire_sequence, rcv_seq, context->next_sequence);
      bad = 0;

      if (p_card.addr EQUALS (0x7f & p->addr))
      {
        /*
          if we're the ACU and it's from the correct source then the sequence number should 
          match
        */
        if ((role EQUALS OSDP_ROLE_ACU) && (rcv_seq != context->next_sequence))
          bad = 1;

        // if we're the PD the received sequence number should match the sequence number on the wire
        if ((role EQUALS OSDP_ROLE_PD) && (wire_sequence != context->next_sequence))
          bad = 1;
      };

      if (bad)
      {
        // if we're not just displaying it...
        if ((role != OSDP_ROLE_MONITOR) &&
          (role EQUALS context->role))
        {
          if (wire_sequence != 0)
          {
fprintf(context->log,
"DEBUG: bad seq bcount %d 0=%02x 1=%02x 2=%02x 5=%02x 6=%02x\n",
            osdp_buf.next,
            osdp_buf.buf [0], osdp_buf.buf [1], osdp_buf.buf [2],
            osdp_buf.buf [5], osdp_buf.buf [6]);

            fprintf(context->log, "***sequence number mismatch got %d expected %d\n", msg_sqn, context->next_sequence);
            status = ST_OSDP_BAD_SEQUENCE;
            context->seq_bad++;

            // putting this back (0.91-10)
            context->next_sequence = 0; // if things are messed up start back at the initial sequence
          };
        };
      };
    };

    // make sure it's for me or the config address

    if (context->role EQUALS OSDP_ROLE_PD)
    {
      if ((p_card.addr != (0x7f & p->addr)) && (p->addr != OSDP_CONFIGURATION_ADDRESS))
      {
        if (context->verbosity > 3)
          fprintf (stderr, "addr mismatch for: %02x me: %02x\n",
            p->addr, p_card.addr);
        status = ST_NOT_MY_ADDR;
      };
    };

    // check the MAC if it's secure channel formatted

    if (status EQUALS ST_OK)
    {
      if (msg_scb != 0)
      {
        // skip hash check if we're in monitor mode.
        // rolling hash/mac calculation gets screwed up...

        if (role != OSDP_ROLE_MONITOR)
        {
          status = oo_hash_check(context, m->ptr, sec_block_type,
            m->crc_check-4, hashable_length);
          if (status EQUALS ST_OK)
            status = osdp_decrypt_payload(context, m);
          if (status != ST_OK)
            fprintf(context->log,
              "Payload decryption failed, status %d.\n", status);
        };
        if (status != ST_OK)
        {
          if (context->verbosity > 3)
            fprintf(context->log,
              "  ..Secure Channel Hash check failed (%d).\n", status);
        };
      }; 
    };

    if ((context->verbosity > 2) || (m_dump > 0))
    {
      char cmd_rep_tag [1024];
      char log_line [3*1024]; // 'cause contents could be 1k already
      char tlogmsg [1024];


      strcpy(cmd_rep_tag,
        osdp_command_reply_to_string(returned_hdr->command, m->direction));

      // print "IEC" details of message
      (void)oosdp_message_header_print(context, m, tlogmsg);
      if (((returned_hdr->command != OSDP_POLL) &&
        (returned_hdr->command != OSDP_ACK)) ||
        (context->verbosity > 3))
      {
        fprintf (context->log, "%s\n", tlogmsg);
        tlogmsg [0] = 0;
        if (context->verbosity > 3)
          dump_buffer_log(context, "  Raw input: ", m->ptr, m->lth);
      };

      sprintf (log_line, "  Pkt %04d Msg %s %s", context->packets_received, cmd_rep_tag, tlogmsg);

      {
        char scb_tag[1024];
        char check_tag [1024];

        if (msg_check_type EQUALS 4)
          sprintf(check_tag, "Check:CRC(%04x)",
            *(1+m->crc_check) << 8 | *(m->crc_check));
        else
          strcpy(check_tag, "Check:Cksum");
        strcpy(scb_tag, "");
        if (msg_scb)
          strcpy(scb_tag, "Sec block present;");

        sprintf (tlogmsg2, " A:%02x Lth:%d. S:%02x %s %s",
          (0x7F & p->addr), (p->len_msb)*256+(p->len_lsb),
          msg_sqn, check_tag, scb_tag);

      };
      strcat (log_line, tlogmsg2);
      if (((returned_hdr->command != OSDP_POLL) &&
        (returned_hdr->command != OSDP_ACK)) ||
        (context->verbosity > 3))
      {
        fprintf (context->log, "%s\n", log_line);
        fflush (context->log);
      };
      tlogmsg [0] = 0; tlogmsg2 [0] = 0;
    };
  };
  if (status EQUALS ST_OK)
  {
    /*
      at this point we think it's a whole well-formed frame.  might not be for
      us but it's a frame.
    */
    context->packets_received ++;

    if (context->role EQUALS OSDP_ROLE_PD)
    {
      // for the PD, go ahead and dump the trace buffers now.
      if (context->verbosity > 3)
        osdp_trace_dump(context, 1);
      else
        osdp_trace_dump(context, 0);
    };
    if (context->role EQUALS OSDP_ROLE_MONITOR)
    {
      int i;
      char temps [4096];
      char octet_string [1024];

      temps[0] = 0;
      for (i=0; i<m->lth; i++)
      {
        sprintf(octet_string, " %02x", *(m->ptr+i));
        strcat(temps, octet_string);
      };
      if (context->trace & 1)
        strcpy(trace_in_buffer, temps);
      if (context->verbosity > 3)
        osdp_trace_dump(context, 1);
      else
        osdp_trace_dump(context, 0);

      (void)monitor_osdp_message (context, m);

      status = ST_MONITOR_ONLY;
    };
  };

  // specifically for bad sequence, call it that they did respond so life can proceed.

  if (status EQUALS ST_OSDP_BAD_SEQUENCE)
  {
    if (context->verbosity > 3)
      fprintf(context->log, "  ...accepting bad sequence as a response\n");
    context->last_was_processed = 1;
  };

  // if there was an error dump the log buffer

  if ((status != ST_OK) && (status != ST_MSG_TOO_SHORT) &&
    (status != ST_NOT_MY_ADDR))
  {
    // if parse failed report the status code
    if ((context->verbosity > 3) && (status != ST_MONITOR_ONLY))
    {
      fflush (context->log);
      fprintf (context->log,
        "Message input parsing failed, status %d\n", status);
    };
  };
  return (status);

} /* osdp_parse_message */

