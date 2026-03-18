/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "vic20.h"
#include "../engine.h"
#include "../ta-log.h"
#include <fmt/printf.h>
#include <array>
#include <vector>

void DivExportVIC20::run() {
  int VIC20=-1;
  int IGNORED=0;

  for (int i=0; i<e->song.systemLen; i++) {
    if (e->song.system[i]==DIV_SYSTEM_VIC20) {
      if (VIC20>=0) {
        IGNORED++;
        logAppendf("ignoring duplicate VIC20 chip id %d",i);
        continue;
      }
      VIC20=i;
      logAppendf("VIC20 detected as chip id %d",i);
    } else {
      IGNORED++;
      logAppendf("ignoring chip id %d, system id %d",i,(int)e->song.system[i]);
    }
  }

  if (VIC20<0) {
    logAppend("ERROR: could not find VIC20 chip");
    failed=true;
    running=false;
    return;
  }
  if (IGNORED>0) {
    logAppendf("WARNING: ignoring %d unsupported chip%c",IGNORED,IGNORED>1?'s':' ');
  }

  std::vector<std::array<uint8_t,5>> frames;
  uint32_t loopFrame=0;
  bool hasLoop=false;

  e->stop();
  e->repeatPattern=false;
  e->setOrder(0);

  logAppend("playing and logging register writes...");

  e->synchronizedSoft([&]() {
    double origRate=e->got.rate;

    e->calcSongTimestamps();
    int loopOrder=e->curSubSong->ts.loopStart.order;
    int loopRow=e->curSubSong->ts.loopStart.row;
    logAppendf("loop point: order %d row %d",loopOrder,loopRow);

    e->curOrder=0;
    e->freelance=false;
    e->playing=false;
    e->extValuePresent=false;
    e->remainingLoops=-1;

    e->playSub(false);

    std::array<uint8_t,5> curr={0,0,0,0,0};
    bool done=false;
    bool loopMarked=false;

    e->disCont[VIC20].dispatch->toggleRegisterDump(true);

    while (!done) {
      if (mustAbort) { done=true; break; }

      if (e->nextTick(false,true)||!e->playing) {
        done=true;
        for (int i=0; i<e->song.systemLen; i++) {
          e->disCont[i].dispatch->getRegisterWrites().clear();
        }
        break;
      }

      std::vector<DivRegWrite>& writes=e->disCont[VIC20].dispatch->getRegisterWrites();
      for (DivRegWrite& w: writes) {
        int reg=(int)(w.addr&0xFF);
        if (reg>=0x0A && reg<=0x0E) {
          curr[reg-0x0A]=(uint8_t)w.val;
        }
      }
      writes.clear();

      if (!loopMarked) {
        if (e->curOrder==loopOrder && e->curRow==loopRow) {
          loopFrame=(uint32_t)frames.size();
          loopMarked=true;
          hasLoop=true;
        }
      }
      frames.push_back(curr);
    }

    e->got.rate=origRate;
    e->disCont[VIC20].dispatch->toggleRegisterDump(false);
    e->remainingLoops=-1;
    e->playing=false;
    e->freelance=false;
    e->extValuePresent=false;
  });

  logAppend("writing data...");
  progress[0].amount=0.9f;

  auto w=new SafeWriter;
  w->init();

  // header: magic "VIC\0"
  w->writeC('V'); w->writeC('I'); w->writeC('C'); w->writeC(0);

  // version
  w->writeC(1);

  // title (32 bytes null-padded)
  {
    char buf[32]; memset(buf,0,32);
    strncpy(buf,e->song.name.c_str(),31);
    w->write(buf,32);
  }

  // author (32 bytes null-padded)
  {
    char buf[32]; memset(buf,0,32);
    strncpy(buf,e->song.author.c_str(),31);
    w->write(buf,32);
  }

  // tick rate (float)
  float rate=e->curSubSong->hz;
  w->write(&rate,4);

  // loop frame index (uint32, 0xFFFFFFFF = no loop)
  uint32_t loopIdx=hasLoop?loopFrame:0xFFFFFFFF;
  w->writeI((int)loopIdx);

  // frame count (uint32) at offset 77
  uint32_t frameCount=(uint32_t)frames.size();
  w->writeI((int)frameCount);

  // clock select byte: 0=NTSC, 1=PAL
  uint8_t clockSel=(uint8_t)e->song.systemFlags[VIC20].getInt("clockSel",0);
  w->writeC(clockSel);
  // padding
  w->writeC(0); w->writeC(0);

  // register data: 5 bytes per frame
  for (auto& f: frames) {
    w->write(f.data(),5);
  }

  // end marker: 5 zero bytes
  uint8_t end[5]={0,0,0,0,0};
  w->write(end,5);

  output.push_back(DivROMExportOutput("export.vic",w));

  progress[0].amount=1.0f;
  logAppendf("done! %d frames written",(int)frames.size());

  running=false;
}

bool DivExportVIC20::go(DivEngine* eng) {
  progress[0].name="Progress";
  progress[0].amount=0.0f;
  e=eng;
  running=true;
  failed=false;
  mustAbort=false;
  exportThread=new std::thread(&DivExportVIC20::run,this);
  return true;
}

void DivExportVIC20::wait() {
  if (exportThread!=NULL) {
    logV("waiting for VIC20 export thread...");
    exportThread->join();
    delete exportThread;
    exportThread=NULL;
  }
}

void DivExportVIC20::abort() {
  mustAbort=true;
  wait();
}

bool DivExportVIC20::isRunning() {
  return running;
}

bool DivExportVIC20::hasFailed() {
  return failed;
}

DivROMExportProgress DivExportVIC20::getProgress(int index) {
  if (index<0||index>1) return progress[1];
  return progress[index];
}
