/* bzflag
 * Copyright (c) 1993 - 2004 Tim Riker
 *
 * This package is free software;  you can redistribute it and/or
 * modify it under the terms of the license found in the file
 * named LICENSE that should have accompanied this file.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "bzfs.h"
#include "CaptureReplay.h"

// Type Definitions
// ----------------

typedef uint16_t u16;

#ifndef _WIN32 //FIXME
typedef uint64_t CRtime;
#else
typedef int CRtime;
#endif

typedef enum {
  FakedPacket  = 0,
  NormalPacket = 1
} RCPacketType;

typedef enum {
  StraightToFile    = 0,
  BufferedCapture   = 1
} CaptureType;

typedef struct CRpacket {
  struct CRpacket *next;
  struct CRpacket *prev;
  u16 fake;
  u16 code;
  int len;
  CRtime timestamp;
  void *data;
} CRpacket;

typedef struct {
  int byteCount;
  int packetCount;
  // into the head, out of the tail
  CRpacket *head; // last packet in 
  CRpacket *tail; // first packet in
//  std::list<CRpacket *> ListTest;
} CRbuffer;


typedef struct {
  unsigned int magic;
  unsigned int version;
  char worldhash[64];
  char padding[1024-2*sizeof(unsigned int)]; // all padding for now
} ReplayHeader;

// Local Variables
// ---------------

static const int DefaultMaxBytes = (16 * 1024 * 1024); // 16 Mbytes
static const int DefaultUpdateRate = 10; // seconds
static const char DefaultFileName[] = "capture.bzr";
static const int ReplayMagic = 0x425A7270; // "BZrp"
static const int ReplayVersion = 0x0001;


static bool Capturing = false;
static CaptureType CaptureMode = BufferedCapture;

static bool Replaying = false;
static bool ReplayMode = false;

static int TotalBytes = 0;
static int MaxBytes = DefaultMaxBytes;
static int UpdateRate = DefaultUpdateRate;

static char *FileName = NULL;


static TimeKeeper StartTime;

static CRbuffer ReplayBuf = {0, 0, NULL, NULL}; // for replaying
static CRbuffer CaptureBuf = {0, 0, NULL, NULL};  // for capturing

static FILE *ReplayFile = NULL;
static FILE *CaptureFile = NULL;

static PlayerInfo PlayerState[MaxPlayers];


// Local Function Prototypes
// -------------------------

static bool saveStates ();
static bool saveTeamScores ();
static bool saveFlagStates ();
static bool savePlayerStates ();

//static bool clearStates ();

static bool saveHeader (ReplayHeader *h, FILE *f);
static bool loadHeader (ReplayHeader *h, FILE *f);

static CRtime getCRtime ();

static void initCRpacket (u16 fake, u16 code, int len, const void *data,
                          CRpacket *p);
static CRpacket *newCRpacket (u16 fake, u16 code, int len, const void *data);
static void addCRpacket (CRbuffer *b, CRpacket *p); // add to head
static CRpacket *delCRpacket (CRbuffer *b); // delete from the tail
static void freeCRbuffer (CRbuffer *buf); // clean it out

static bool saveCRpacket (CRpacket *p, FILE *f);
static CRpacket *loadCRpacket (FILE *f); // makes a new packet

static const char *print_msg_code (u16 code);


// External Dependencies   (from bzfs.cxx)
// ---------------------

extern int numFlags;
extern int numFlagsInAir;
extern FlagInfo *flag;
extern PlayerInfo player[MaxPlayers];
extern uint16_t curMaxPlayers;
extern TeamInfo team[NumTeams];
extern void *getDirectMessageBuffer(void);
extern void directMessage(int playerIndex, u16 code, 
                          int len, const void *msg);
extern void sendMessage(int playerIndex, PlayerId targetPlayer, 
                        const char *message, bool fullBuffer=false);
                        
/****************************************************************************/

// Capture Functions

bool Capture::init ()
{
  if (CaptureFile != NULL) {
    fclose (CaptureFile);
    CaptureFile = NULL;
  }
  if ((FileName != NULL) && (FileName != DefaultFileName)) {
    free (FileName);
    FileName = NULL;
  }
  freeCRbuffer (&CaptureBuf);
  
  MaxBytes = DefaultMaxBytes;
  UpdateRate = DefaultUpdateRate;
  Capturing = false;
  TotalBytes = 0;
  CaptureMode = BufferedCapture;
  
  return true;
}

bool Capture::kill ()
{
  freeCRbuffer (&CaptureBuf);
  if (CaptureFile != NULL) {
    fclose (CaptureFile);
  }
  freeCRbuffer (&ReplayBuf);
  if (ReplayFile != NULL) {
    fclose (ReplayFile);
  }
  
  return true;
}

bool Capture::start ()
{
  Capturing = true;
  return true;
}

bool Capture::stop ()
{
  Capturing = false;
  if (CaptureMode == StraightToFile) {
    Capture::init();
  }
  
  return true;
}

bool Capture::setSize (int Mbytes)
{
  MaxBytes = Mbytes * (1024) * (1024);
  return true;
}

bool Capture::setRate (int seconds)
{
  UpdateRate = seconds;
  return true;
}

bool Capture::sendStats (int playerIndex)
{
  sendMessage (ServerPlayer, playerIndex, "TEST MESSAGE, NO REAL STATS YET");
  return true;
}

bool Capture::saveFile (const char *filename)
{
  ReplayHeader header;
  
  Capture::init();
  Capturing = true;
  CaptureMode = StraightToFile;

  if (filename == NULL) {
    filename = DefaultFileName;
  }
  
  CaptureFile = fopen (filename, "wb");
  if (CaptureFile == NULL) {
    Capture::init();
    return false;
  }
  
  if (!saveHeader (&header, CaptureFile)) {
    Capture::init();
    return false;
  }
  
  if (!saveStates ()) {
    Capture::init();
    return false;
  }
  
  return true;
}

bool Capture::saveBuffer (const char *filename)
{
  filename = filename;
  return true;
}

bool 
Capture::addPacket (u16 code, int len, const void * data, bool fake)
{
  u16 fakeval = NormalPacket;
  if (fake) {
    fakeval = FakedPacket;
  }
  
  if (CaptureMode == BufferedCapture) {
    CRpacket *p = newCRpacket (fakeval, code, len, data);
    p->timestamp = getCRtime();
    addCRpacket (&CaptureBuf, p);
    printf ("Capture::addPacket: buffered %s\n", print_msg_code (code));
  }
  else {
    CRpacket p;
    p.timestamp = getCRtime();
    initCRpacket (fakeval, code, len, data, &p);
    saveCRpacket (&p, CaptureFile);
    printf ("Capture::addPacket: saved %s\n", print_msg_code (code));
  }
  
  return true; 
}

bool Capture::enabled ()
{
  return Capturing;
}

int Capture::getSize ()
{
  return MaxBytes;
}

int Capture::getRate ()
{
  return UpdateRate;
}

const char * Capture::getFileName ()
{
  return FileName;
}
                          
/****************************************************************************/

// Replay Functions

bool Replay::init()
{
  ReplayMode = true;
  return true;
}

bool Replay::kill()
{
  if (ReplayFile != NULL) {
    fclose (ReplayFile);
  }
  return true;
}

bool Replay::enabled()
{
  return ReplayMode;
}

bool Replay::loadFile(const char * filename)
{
  ReplayHeader header;
  if (filename == NULL) {
    filename = DefaultFileName;
  }
  ReplayFile = fopen (filename, "rb");
  if (ReplayFile == NULL) {
    return false;
  }
  if (!loadHeader (&header, ReplayFile)) {
    return false;
  }
  return true;
}

float Replay::nextTime()
{
  return 1000.0f;
}

bool Replay::replaying ()
{
  return Replaying;
}
                          
bool Replay::skip(int seconds)
{
  seconds = seconds;
  return true;
}

bool Replay::sendFileList(int playerIndex)
{
  playerIndex = playerIndex;
  return true;
}

bool Replay::play()
{
  return true;
}

bool Replay::nextPacket (u16 *code, int *len, void **data)
{
  if (CaptureBuf.tail == NULL) {
    return false;
  }
  *code = CaptureBuf.tail->code;
  *len = CaptureBuf.tail->len;
  *data = (void *) CaptureBuf.tail->data;
  
  delCRpacket (&CaptureBuf); // remove from the tail
  
  return true;
}

/****************************************************************************/

// State Management Functions

// The goal is to save all of the states, such that if 
// the packets are simply sent to a clean-state client,
// the client's state will end up looking like the state
// at the time which this function was called.

/* from bzfs / addPlayer()

  if (!player[playerIndex].isBot()) {
    int i;
    if (NetHandler::exists(playerIndex)) {
      sendTeamUpdate(playerIndex);
      sendFlagUpdate(-1, playerIndex);
    }
    for (i = 0; i < curMaxPlayers && NetHandler::exists(playerIndex); i++)
      if (player[i].isPlaying() && i != playerIndex)
        sendPlayerUpdate(i, playerIndex);
  }
*/  

static bool
saveTeamScores ()
{
  int i;
  char bufStart[MaxPacketLen];
  void *buf;
  
  buf = nboPackUByte (bufStart, CtfTeams);
  for (i=0 ; i<CtfTeams; i++) {
    // ubyte for the team number, 3 ushort for scores
    buf = team[i].team.pack(buf);
  }
  
  Capture::addPacket (MsgTeamUpdate, (char*)buf - (char*)bufStart,  bufStart, true);
  
  return true;
}

static bool
saveFlagStates () // look at sendFlagUpdate() in bzfs.cxx ... very similar
{
  int flagIndex;
  char bufStart[MaxPacketLen];
  void *buf;
  
  buf = nboPackUShort(bufStart,0); //placeholder
  int cnt = 0;
  int length = sizeof(uint16_t);
  
printf ("numFlags = %i\n", numFlags);
fflush (stdout);
  
  for (flagIndex = 0; flagIndex < numFlags; flagIndex++) {

printf ("FlagNum = %i\n", flagIndex); fflush (stdout);

    if (flag[flagIndex].flag.status != FlagNoExist) {
      if ((length + sizeof(uint16_t) + FlagPLen) > MaxPacketLen - 2*sizeof(uint16_t)) {
        // packet length overflow
        nboPackUShort(bufStart, cnt);
        Capture::addPacket (MsgFlagUpdate, (char*)buf - (char*)bufStart, bufStart, true);

        cnt = 0;
        length = sizeof(uint16_t);
        buf = nboPackUShort(bufStart,0); //placeholder
      }

      buf = nboPackUShort(buf, flagIndex);
      buf = flag[flagIndex].flag.pack(buf);
      length += sizeof(uint16_t)+FlagPLen;
      cnt++;
    }
  }

  if (cnt > 0) {
    nboPackUShort(bufStart, cnt);
    Capture::addPacket (MsgFlagUpdate, (char*)buf - (char*)bufStart, bufStart, true);
  }
  
  return true;
}

static bool
savePlayerStates ()
{
  int i;
  char bufStart[MaxPacketLen];
  void *buf;

  for (i = 0; i < curMaxPlayers; i++) {
    if (player[i].isPlaying()) {
      PlayerInfo *pPlayer = &player[i];
      buf = nboPackUByte(bufStart, i);
      buf = pPlayer->packUpdate(buf);
      Capture::addPacket (MsgAddPlayer, (char*)buf - (char*)bufStart, bufStart, true);
    }
  }
  
  return true;
}

static bool
saveStates ()
{
  saveTeamScores ();
  saveFlagStates ();
  savePlayerStates ();
  
  return true;
}

/****************************************************************************/

// File Functions

// The replay files should work on different machine
// types, so everything is saved in network byte order.
                          
static bool
saveCRpacket (CRpacket *p, FILE *f)
{
  const int bufsize = sizeof(u16)*2 + sizeof(int);
  char bufStart[bufsize];
  void *buf;

  if (f == NULL) {
    return false;
  }

  buf = nboPackUShort (bufStart, p->fake);
  buf = nboPackUShort (buf, p->code);
  buf = nboPackUInt (buf, p->len);
  fwrite (bufStart, bufsize, 1, f);
  fwrite (&p->timestamp, sizeof (CRtime), 1, f); //FIXME 
  fwrite (p->data, p->len, 1, f);

  fflush (f);//FIXME

  return true;  
}

static CRpacket * //FIXME - totally botched
loadCRpacket (FILE *f)
{
  CRpacket *p;
  const int bufsize = sizeof(u16)*2 + sizeof(int);
  char bufStart[bufsize];
  void *buf;
  
  if (f == NULL) {
    return false;
  }

  fread (bufStart, bufsize, 1, f);
  buf = nboUnpackUShort (bufStart, p->fake);
  buf = nboUnpackUShort (buf, p->code);
  buf = nboUnpackInt (buf, p->len);
  fread (&p->timestamp, sizeof (CRtime), 1, f); //FIXME 
  fread (p->data, p->len, 1, f);
  
  p = loadCRpacket (f); //FIXME - to avoid warnings for now

  return NULL;  
}

static bool
saveHeader (ReplayHeader *h, FILE *f)
{
  const int bufsize = sizeof(ReplayHeader);
  char buffer[bufsize];
  void *buf;
  
  if (f == NULL) {
    return false;
  }

  buf = nboPackUInt (buffer, h->magic);
  buf = nboPackUInt (buf, h->version);
  
  fwrite (buffer, bufsize, 1, f);
  
  return true;
}

static bool
loadHeader (ReplayHeader *h, FILE *f)
{
  const int bufsize = sizeof(ReplayHeader);
  char buffer[bufsize];
  void *buf;
  
  if (f == NULL) {
    return false;
  }

  fread (buffer, bufsize, 1, f);
  
  buf = nboUnpackUInt (buffer, h->magic);
  buf = nboUnpackUInt (buf, h->version);
  memcpy (h->worldhash, buf, sizeof (h->worldhash));
  
  return true;
}
                          
/****************************************************************************/

// Buffer Functions

static void
initCRpacket (u16 fake, u16 code, int len, const void *data, CRpacket *p)
{
  p->fake = fake;
  p->code = code;
  p->len = len;
  (const void *) p->data = data; // FIXME - dirty little trick
}

static CRpacket *
newCRpacket (u16 fake, u16 code, int len, const void *data)
{
  CRpacket *p = (CRpacket *) malloc (sizeof (CRpacket));
  
  p->next = NULL;
  p->prev = NULL;

  p->data = malloc (len);
  if (data != NULL) {
    memcpy (p->data, data, len);
  }
  initCRpacket (fake, code, len, data, p);

  return p;
}

static void
addCRpacket (CRbuffer *b, CRpacket *p)
{
  if (b->head != NULL) {
    b->head->next = p;
  }
  else {
    b->tail = p;
  }
  p->prev = b->head;
  p->next = NULL;
  b->head = p;
  
  b->byteCount = b->byteCount + p->len; //FIXME +8?
  b->packetCount++;
  
  return;
}

static CRpacket *
delCRpacket (CRbuffer *b)
{
  CRpacket *p = b->tail;
  if (b->tail == NULL) {
    b->head = NULL;
    b->tail = NULL;
    b->byteCount = 0;
    b->packetCount = 0;
    return NULL;
  }
  
  b->byteCount = b->byteCount - p->len;
  b->packetCount--;
  
  
  if (b->tail->next != NULL) {
    p->next->prev = NULL;
  }
  p = b->tail;
  if (b->head == b->tail) {
    b->head = NULL;
    b->tail = NULL;
    b->byteCount = 0;
    b->packetCount = 0;
  }
  
  b = b;
  return p;
}

static void
freeCRbuffer (CRbuffer *b)
{
  CRpacket *p, *ptmp;
  
  p = b->tail;

  while (p != NULL) {
    ptmp = p->next;
    free (p->data);
    free (p);
    p = ptmp;
  }
  
  return;
}

/****************************************************************************/

// Timing Functions

static CRtime
getCRtime ()
{
#ifndef _WIN32
  CRtime now;
  struct timeval tv;
  gettimeofday (&tv, NULL);
  now = (CRtime)tv.tv_sec *  (CRtime)1000000;
  now = now + (CRtime)tv.tv_usec;
  return now;
#else
  return 0; // FIXME - Windows is currently broken
#endif
}

/****************************************************************************/

// FIXME - for debugging

static const char *
print_msg_code (u16 code)
{

#define STRING_CASE(x)  \
  case x: return #x

  switch (code) {
      STRING_CASE (MsgNull);
      STRING_CASE (MsgAccept);
      STRING_CASE (MsgAlive);
      STRING_CASE (MsgAddPlayer);
      STRING_CASE (MsgAudio);
      STRING_CASE (MsgCaptureFlag);
      STRING_CASE (MsgDropFlag);
      STRING_CASE (MsgEnter);
      STRING_CASE (MsgExit);
      STRING_CASE (MsgFlagUpdate);
      STRING_CASE (MsgGrabFlag);
      STRING_CASE (MsgGMUpdate);
      STRING_CASE (MsgGetWorld);
      STRING_CASE (MsgKilled);
      STRING_CASE (MsgMessage);
      STRING_CASE (MsgNewRabbit);
      STRING_CASE (MsgNegotiateFlags);
      STRING_CASE (MsgPause);
      STRING_CASE (MsgPlayerUpdate);
      STRING_CASE (MsgQueryGame);
      STRING_CASE (MsgQueryPlayers);
      STRING_CASE (MsgReject);
      STRING_CASE (MsgRemovePlayer);
      STRING_CASE (MsgShotBegin);
      STRING_CASE (MsgScore);
      STRING_CASE (MsgScoreOver);
      STRING_CASE (MsgShotEnd);
      STRING_CASE (MsgSuperKill);
      STRING_CASE (MsgSetVar);
      STRING_CASE (MsgTimeUpdate);
      STRING_CASE (MsgTeleport);
      STRING_CASE (MsgTransferFlag);
      STRING_CASE (MsgTeamUpdate);
      STRING_CASE (MsgVideo);
      STRING_CASE (MsgWantWHash);

      STRING_CASE (MsgUDPLinkRequest);
      STRING_CASE (MsgUDPLinkEstablished);
      STRING_CASE (MsgServerControl);
      STRING_CASE (MsgLagPing);

    default:
      static char buf[32];
      sprintf (buf, "MagUnknown: 0x%04X", code);
      return buf;
  }
}

/****************************************************************************/

// Local Variables: ***
// mode: C++ ***
// tab-width: 8 ***
// c-basic-offset: 2 ***
// indent-tabs-mode: t ***
// End: ***
// ex: shiftwidth=2 tabstop=8

