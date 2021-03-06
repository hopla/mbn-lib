/****************************************************************************
**
** Copyright (C) 2009 D&R Electronica Weesp B.V. All rights reserved.
**
** This file is part of the Axum/MambaNet digital mixing system.
**
** This file may be used under the terms of the GNU General Public
** License version 2.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of
** this file.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <ipexport.h>
#include <pcap.h>
#include <Packet32.h>

#include "mbn.h"


#define BUFFERSIZE 512
#define ADDLSTSIZE 1000


/* dynamic loading of wpcap.dll functions */
static int     (*p_pcap_findalldevs) (pcap_if_t **, char *);
static void    (*p_pcap_freealldevs) (pcap_if_t *);
static void    (*p_pcap_close) (pcap_t *);
static pcap_t* (*p_pcap_open_live) (const char *, int, int, int, char *);
static int     (*p_pcap_compile) (pcap_t *, struct bpf_program *, char *, int, bpf_u_int32);
static int     (*p_pcap_setfilter) (pcap_t *, struct bpf_program *);
static void    (*p_pcap_freecode) (struct bpf_program *);
static char*   (*p_pcap_geterr) (pcap_t *);
static int     (*p_pcap_next_ex) (pcap_t *, struct pcap_pkthdr **, const u_char **);
static int     (*p_pcap_sendpacket) (pcap_t *, u_char *, int);
static char imported = 0;

static char*     (*p_packet32_packetgetversion) (void);
static LPADAPTER (*p_packet32_packetopenadapter) (char *);
static void      (*p_packet32_packetcloseadapter) (LPADAPTER);
static int       (*p_packet32_packetrequest) (LPADAPTER, int, void *);

int import_pcap() {
  if(imported)
    return 1;
  HANDLE dll;
  if((dll = LoadLibrary("wpcap.dll")) <= (HANDLE)HINSTANCE_ERROR)
    return 0;
  p_pcap_findalldevs = (int     (*)(pcap_if_t **, char *)) GetProcAddress(dll, "pcap_findalldevs");
  p_pcap_freealldevs = (void    (*)(pcap_if_t *)) GetProcAddress(dll, "pcap_freealldevs");
  p_pcap_close       = (void    (*)(pcap_t *)) GetProcAddress(dll, "pcap_close");
  p_pcap_open_live   = (pcap_t *(*)(const char *, int, int, int, char *)) GetProcAddress(dll, "pcap_open_live");
  p_pcap_compile     = (int     (*)(pcap_t *, struct bpf_program *, char *, int, bpf_u_int32)) GetProcAddress(dll, "pcap_compile");
  p_pcap_setfilter   = (int     (*)(pcap_t *, struct bpf_program *)) GetProcAddress(dll, "pcap_setfilter");
  p_pcap_freecode    = (void    (*)(struct bpf_program *)) GetProcAddress(dll, "pcap_freecode");
  p_pcap_geterr      = (char   *(*)(pcap_t *)) GetProcAddress(dll, "pcap_geterr");
  p_pcap_next_ex     = (int     (*)(pcap_t *, struct pcap_pkthdr **, const u_char **)) GetProcAddress(dll, "pcap_next_ex");
  p_pcap_sendpacket  = (int     (*)(pcap_t *, u_char *, int)) GetProcAddress(dll, "pcap_sendpacket");

  if((dll = LoadLibrary("Packet.dll")) <= (HANDLE)HINSTANCE_ERROR)
    return 0;
  p_packet32_packetgetversion   = (char      *(*)(void)) GetProcAddress(dll, "PacketGetVersion");
  p_packet32_packetopenadapter  = (LPADAPTER  (*)(char *)) GetProcAddress(dll, "PacketOpenAdapter");
  p_packet32_packetcloseadapter = (void       (*)(LPADAPTER)) GetProcAddress(dll, "PacketCloseAdapter"); 
  p_packet32_packetrequest      = (int        (*)(LPADAPTER, int, void *)) GetProcAddress(dll, "PacketRequest");

  return ++imported;
}

#define pcap_findalldevs p_pcap_findalldevs
#define pcap_freealldevs p_pcap_freealldevs
#define pcap_close       p_pcap_close
#define pcap_open_live   p_pcap_open_live
#define pcap_compile     p_pcap_compile
#define pcap_setfilter   p_pcap_setfilter
#define pcap_freecode    p_pcap_freecode
#define pcap_geterr      p_pcap_geterr
#define pcap_next_ex     p_pcap_next_ex
#define pcap_sendpacket  p_pcap_sendpacket



struct pcapdat {
  pcap_t *pc;
  LPADAPTER pa;
  pthread_t thread;
  char thread_run;
  unsigned char mymac[6];
  unsigned char macs[ADDLSTSIZE][6];
};


void stop_pcap(struct mbn_interface *);
void free_pcap(struct mbn_interface *);
void free_pcap_addr(struct mbn_interface *, void *);
int init_pcap(struct mbn_interface *, char *);
void *receive_packets(void *ptr);
int wpcaptransmit(struct mbn_interface *, unsigned char *, int, void *, char *);


struct mbn_if_ethernet * MBN_EXPORT mbnEthernetIFList(char *err) {
  struct mbn_if_ethernet *e, *l, *n;
  pcap_if_t *devs, *d;
  pcap_addr_t *a;
  char mac[6];
  int suc;
  unsigned long alen;

  e = NULL;
  if(!import_pcap()) {
    sprintf(err, "Couldn't load wpcap.dll, get it from http://winpcap.org/");
    return e;
  }

  if(pcap_findalldevs(&devs, err+27) < 0) {
    memcpy((void *)err, (void *)"Can't get list of devices: ", 27);
    return e;
  }
  if(devs == NULL) {
    sprintf(err, "No devices found");
    return e;
  }

  for(d=devs; d!=NULL; d=d->next) {
    suc = 0;
    for(a=d->addresses; a!=NULL; a=a->next) {
      if(a->addr == NULL && a->addr->sa_family != AF_INET)
        continue;
      alen = 6;
      if(SendARP((IPAddr)((struct sockaddr_in *)a->addr)->sin_addr.s_addr, 0, (unsigned long *)mac, &alen) == NO_ERROR) {
        suc = 1;
        break;
      }
    }
    if(!suc)
      continue;
    n = calloc(1, sizeof(struct mbn_if_ethernet));
    n->name = malloc(strlen(d->name)+1);
    memcpy((void *)n->name, (void *)d->name, strlen(d->name)+1);
    memcpy((void *)n->addr, (void *)mac, 6);
    if(d->description) {
      n->desc = malloc(strlen(d->description)+1);
      memcpy((void *)n->desc, (void *)d->description, strlen(d->description)+1);
    }
    if(e == NULL)
      e = n;
    else
      l->next = n;
    l = n;
  }
  pcap_freealldevs(devs);

  if(e == NULL)
    sprintf(err, "No devices found");

  return e;
}


void MBN_EXPORT mbnEthernetIFFree(struct mbn_if_ethernet *list) {
  struct mbn_if_ethernet *n;
  while(list != NULL) {
    n = list->next;
    if(list->desc)
      free(list->desc);
    free(list->name);
    free(list);
    list = n;
  }
}


struct mbn_interface * MBN_EXPORT mbnEthernetOpen(char *ifname, char *err) {
  struct mbn_interface *itf;
  struct pcapdat *dat;
  pcap_addr_t *a;
  pcap_if_t *devs, *d;
  pcap_t *pc = NULL;
  LPADAPTER pa = NULL;
  struct bpf_program fp;
  int i, suc, error = 0;
  unsigned long alen;

  if(!import_pcap()) {
    sprintf(err, "Couldn't load wpcap.dll, get it from http://winpcap.org/");
    return NULL;
  }

  if(ifname == NULL) {
    sprintf(err, "No interface specified");
    return NULL;
  }

  itf = (struct mbn_interface *)calloc(1, sizeof(struct mbn_interface));
  dat = (struct pcapdat *)calloc(1, sizeof(struct pcapdat));
  itf->data = (void *) dat;


  /* get device list */
  if(pcap_findalldevs(&devs, err+27) < 0) {
    memcpy((void *)err, (void *)"Can't get list of devices: ", 27);
    error++;
  }
  if(!error && devs == NULL) {
    sprintf(err, "No devices found");
    error++;
  }

  /* open device */
  for(d=devs; d!=NULL; d=d->next)
    if(strcmp(d->name, ifname) == 0)
      break;
  if(!error && d == NULL) {
    sprintf(err, "Selected device not found");
    error++;
  }
  if(!error && (pc = pcap_open_live(d->name, BUFFERSIZE, 0, 1, err+25)) == NULL) {
    memcpy((void *)err, (void *)"Couldn't open interface: ", 25);
    error++;
  }

  if(!error && (pa = p_packet32_packetopenadapter(d->name)) == NULL) {
    memcpy((void *)err, (void *)"Couldn't open adapter: ", 23);
    error++; 
  }

  /* get MAC address */
  suc = 0;
  if(!error) {
    for(a=d->addresses; a!=NULL; a=a->next) {
      if(a->addr != NULL && a->addr->sa_family == AF_INET) {
        alen = 6;
        if((i = SendARP((IPAddr)((struct sockaddr_in *)a->addr)->sin_addr.s_addr, 0, (unsigned long *)dat->mymac, &alen)) != NO_ERROR) {
          sprintf(err, "SendARP failed with code %d", i);
          error++;
        } else
          suc = 1;
      }
    }
    if(!suc) {
      sprintf(err, "Couldn't get MAC address");
      error++;
    }
  }

  /* set filter for MambaNet */
  if(!error && pcap_compile(pc, &fp, "ether proto 34848", 1, 0) == -1) {
    sprintf(err, "Can't compile filter: %s", pcap_geterr(pc));
    error++;
  }
  if(!error && pcap_setfilter(pc, &fp) == -1) {
    sprintf(err, "Can't set filter: %s", pcap_geterr(pc));
    error++;
  }
  if(!error)
    pcap_freecode(&fp);

  if(devs != NULL)
    pcap_freealldevs(devs);

  if(error) {
    free(dat);
    free(itf);
    return NULL;
  }

  dat->pc = pc;
  dat->pa = pa;
  itf->cb_stop = stop_pcap;
  itf->cb_free = free_pcap;
  itf->cb_init = init_pcap;
  itf->cb_free_addr = free_pcap_addr;
  itf->cb_transmit = wpcaptransmit;

  return itf;
}


void stop_pcap(struct mbn_interface *itf) {
  struct pcapdat *dat = (struct pcapdat *)itf->data;
  int i;

/*  pcap_close(dat->pc);*/

  for(i=0; !dat->thread_run; i++) {
    if(i > 5)
      break;
    Sleep(1000);
  }
  pthread_cancel(dat->thread);
  pthread_join(dat->thread, NULL);
}

void free_pcap(struct mbn_interface *itf) {
  struct pcapdat *dat = (struct pcapdat *)itf->data;
  int i;

  p_packet32_packetcloseadapter(dat->pa);

  pcap_close(dat->pc);

  for(i=0; !dat->thread_run; i++) {
    if(i > 5)
      break;
    Sleep(1000);
  }
  pthread_cancel(dat->thread);
  pthread_join(dat->thread, NULL);
  free(dat);
  free(itf);
}


void free_pcap_addr(struct mbn_interface *itf, void *arg) {
  mbnWriteLogMessage(itf, "Remove Ethernet addresse %02X:%02X:%02X:%02X:%02X:%02X", ((unsigned char *)arg)[0],
                                                                                    ((unsigned char *)arg)[1],
                                                                                    ((unsigned char *)arg)[2],
                                                                                    ((unsigned char *)arg)[3],
                                                                                    ((unsigned char *)arg)[4],
                                                                                    ((unsigned char *)arg)[5]);
  memset(arg, 0, 6);
}


int init_pcap(struct mbn_interface *itf, char *err) {
  struct pcapdat *dat = (struct pcapdat *)itf->data;
  int i;

  /* create thread to wait for packets */
  if((i = pthread_create(&(dat->thread), NULL, receive_packets, (void *) itf)) != 0) {
    sprintf(err, "Can't create thread: %s (%d)", strerror(i), i);
    return 1;
  }
  return 0;
}


/* Waits for input from network */
void *receive_packets(void *ptr) {
  struct mbn_interface *itf = (struct mbn_interface *)ptr;
  struct pcapdat *dat = (struct pcapdat *) itf->data;
  struct pcap_pkthdr *hdr;
  void *ifaddr, *hwaddr;
  unsigned char *buffer;
  char err[MBN_ERRSIZE];
  int i,j;

  dat->thread_run = 1;

  while(1) {
    pthread_testcancel();

    /* wait for/read packet */
    i = pcap_next_ex(dat->pc, &hdr, (const u_char **)&buffer);
    if(i < 0) {
      sprintf(err, "Couldn't read packet: %s", pcap_geterr(dat->pc));
      mbnInterfaceReadError(itf, err);
      break;
    }
    if(i == 0)
      continue;

    /* get HW address pointer */
    hwaddr = ifaddr = NULL;
    for(j=0; j<ADDLSTSIZE-1; j++) {
      if(hwaddr == NULL && memcmp(dat->macs[j], "\0\0\0\0\0\0", 6) == 0)
        hwaddr = dat->macs[j];
      if(memcmp(dat->macs[j], buffer+6, 6) == 0) {
        ifaddr = dat->macs[j];
        break;
      }
    }
    if(ifaddr == NULL) {
      ifaddr = hwaddr;
      memcpy(ifaddr, buffer+6, 6);

      mbnWriteLogMessage(itf, "Remove Ethernet address %02X:%02X:%02X:%02X:%02X:%02X", ((unsigned char *)hwaddr)[0],
                                                                                       ((unsigned char *)hwaddr)[1],
                                                                                       ((unsigned char *)hwaddr)[2],
                                                                                       ((unsigned char *)hwaddr)[3],
                                                                                       ((unsigned char *)hwaddr)[4],
                                                                                       ((unsigned char *)hwaddr)[5]);
    }
    /* forward to mbn */
    mbnProcessRawMessage(itf, buffer+14, (int)hdr->caplen-14, ifaddr);
  }

  return NULL;
}


int wpcaptransmit(struct mbn_interface *itf, unsigned char *buf, int len, void *ifaddr, char *err) {
  struct pcapdat *dat = (struct pcapdat *) itf->data;
  unsigned char send[MBN_MAX_MESSAGE_SIZE];

  if(ifaddr != 0)
    memcpy((void *)send, ifaddr, 6);
  else
    memset((void *)send, 0xFF, 6);
  memcpy((void *)send+6, (void *)dat->mymac, 6);
  send[12] = 0x88;
  send[13] = 0x20;
  memcpy((void *)send+14, (void *)buf, len);

  if(pcap_sendpacket(dat->pc, send, len+14) < 0) {
    sprintf(err, "Can't send packet: %s", pcap_geterr(dat->pc));
    return 1;
  }
  return 0;
}

#define OID_GEN_MEDIA_CONNECT_STATUS 0x00010114

char MBN_EXPORT mbnEthernetMIILinkStatus(struct mbn_interface *itf, char *err) {
  struct pcapdat *dat = (struct pcapdat *) itf->data;
  unsigned char oid_storage[1024];

  PPACKET_OID_DATA oid_data = (PPACKET_OID_DATA)oid_storage;

  memset(oid_storage, 0, 1024);
  oid_data->Oid = OID_GEN_MEDIA_CONNECT_STATUS;
  oid_data->Length = 1024-sizeof(oid_data->Oid);

  p_packet32_packetrequest(dat->pa, 0, oid_data);

  return (oid_data->Data[0]&0x01) ? 0 : 1;
  err = NULL;
}


