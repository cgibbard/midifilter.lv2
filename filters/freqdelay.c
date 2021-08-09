MFD_FILTER(freqdelay)

#ifdef MX_TTF

  mflt:freqdelay
  TTF_DEF("MIDI Frequency Delay", "MIDI Frequency Delay", ; atom:supports time:Position)
  , TTF_IPORT(0, "channelf", "Filter Channel", 0, 16, 0,
      PORTENUMZ("Any")
      DOC_CHANF)
  , TTF_IPORT( 1, "bpmsrc",  "BPM source", 0, 1, 1,
      lv2:scalePoint [ rdfs:label "Control Port" ; rdf:value 0 ] ;
      lv2:scalePoint [ rdfs:label "Plugin Host (if available)" ; rdf:value 1 ] ;
      lv2:portProperty lv2:integer; lv2:portProperty lv2:enumeration;
      )
  , TTF_IPORT(2, "delayBPM",  "BPM", 1.0, 280.0,  120.0, units:unit units:bpm;
      rdfs:comment "base unit for the delay-time")
  , TTF_IPORT(3, "taps",  "Repeats", 0, 64, 3, lv2:portProperty lv2:integer;
      lv2:scalePoint [ rdfs:label "until note-off" ; rdf:value 0 ] ;
      rdfs:comment "Number of repeats")
  , TTF_IPORT(4, "velocityadj",  "velocity ramp", -64.0, 64.0, -10.0,
      rdfs:comment "Velocity change per repeat")
  ; rdfs:comment "This effect repeats notes N times. Where N is either a fixed number or unlimited as long as a given key is pressed. BPM and delay-time variable allows tempo-ramps. The frequency of repeats is based on the frequency of the note."
  .

#elif defined MX_CODE

static void
filter_cleanup_freqdelay(MidiFilter* self)
{
  free(self->memQ);
}

static inline void filter_freqdelay_panic(MidiFilter* self, const uint8_t c, const uint32_t tme) {
  int i,k;
  const int max_delay = self->memI[20];
  for (i=0; i < max_delay; ++i) {
    if (self->memQ[i].size == 3
        && (self->memQ[i].buf[0] & 0xf0) != 0xf0
        && (self->memQ[i].buf[0] & 0x0f) != c)
      continue;
    self->memQ[i].size = 0;
  }

  self->memI[c] = 8192; // pitch bend
  self->memF[c] = 1; // grid
  for (k=0; k < 127; ++k) {
    if (self->memCS[c][k]) {
      uint8_t buf[3];
      buf[0] = MIDI_NOTEOFF | c;
      buf[1] = k; buf[2] = 0;
      forge_midimessage(self, tme, buf, 3);
    }
    self->memCS[c][k] = 0; // count note-on per key post-delay
    self->memCM[c][k] = 0; // remember velocity of note-on
    self->memCI[c][k] = -1; // remember time of note-on
  }
}

void
filter_midi_freqdelay(MidiFilter* self,
    const uint32_t tme,
    const uint8_t* const buffer,
    const uint32_t size)
{
  int i;
  float bpm = MAX(*self->cfg[2], 1.0);
  if (*self->cfg[1] && (self->available_info & NFO_BPM)) {
    bpm = self->bpm;
  }
  if (bpm <= 0) bpm = 60;

  if (midi_is_panic(buffer, size)) {
    filter_freqdelay_panic(self, buffer[0]&0x0f, tme);
  }

  const uint8_t chs = midi_limit_chn(floorf(*self->cfg[0]) -1);
  const uint8_t chn = buffer[0] & 0x0f;
  uint8_t mst = buffer[0] & 0xf0;

  if (mst == MIDI_PITCHBEND) {
    self->memI[chn] = (int)(buffer[1] & 0x7f) + (((int)(buffer[2] & 0x7f)) << 7);
  }

  if (size != 3
      || !(mst == MIDI_NOTEON || mst == MIDI_NOTEOFF || mst == MIDI_POLYKEYPRESSURE)
      || !(floorf(*self->cfg[0]) == 0 || chs == chn)
     )
  {
    forge_midimessage(self, tme, buffer, size);
    return;
  }

  if ((self->memI[22] + 1) % self->memI[20] == self->memI[21]) {
    forge_midimessage(self, tme, buffer, size);
    return;
  }

  ///const float grid = RAIL((*self->cfg[3]), 1/256.0, 4.0);
  const double samples_per_beat = 60.0 / bpm * self->samplerate;
  const uint8_t key = buffer[1] & 0x7f;
  const uint8_t vel = buffer[2] & 0x7f;

  uint8_t buf[3];
  memcpy(buf, buffer, 3);

  /* treat note-on w/velocity==0 as note-off */
  if (mst == MIDI_NOTEON && vel == 0) {
    mst = MIDI_NOTEOFF;
    buf[0] = MIDI_NOTEOFF | chn;
  }

  if (mst == MIDI_NOTEON) {
    printf("chn: %d, key: %d, bend: %d\n", chn, key, self->memI[chn]);
    const double pbfrac = ((double) (self->memI[chn])) * 4 / 16384 - 2;
    printf("pbfrac: %f\n", pbfrac);
    const double freq = pow(2,(((double) key - 69)/12 + pbfrac)); // A440 = quarter note delay
    printf("Delay frequency: %f, time: %f beats\n", freq, (1/freq));
    self->memF[chn] = (float) 1/freq;
    self->memCI[chn][key] = tme + rint(self->memF[chn] * samples_per_beat);
    self->memCM[chn][key] = vel;
    if (self->memCS[chn][key] == 0) {
        self->memCS[chn][key]++; // key active
        forge_midimessage(self, tme, buffer, size);
    }
  }
  else if (mst == MIDI_NOTEOFF) {
    self->memCI[chn][key] = -1;
    self->memCM[chn][key] = 0;
    if (self->memCS[chn][key] > 0) {
      self->memCS[chn][key]--;
      if (self->memCS[chn][key] == 0) {
        forge_midimessage(self, tme, buffer, size);
      }
    }

  } else {
    forge_midimessage(self, tme, buffer, size);
  }

  for (i=0; i < RAIL(*self->cfg[3], 0, 128); ++i) {
    int delay = rint(self->memF[chn] * samples_per_beat * (i+1.0));
    if (mst == MIDI_NOTEON) {
      buf[2] = RAIL(rintf(vel + (i+1.0) * (*self->cfg[4])), 1, 127);
    } else {
      buf[2] = vel;
    }
    MidiEventQueue *qm = &(self->memQ[self->memI[22]]);
    memcpy(qm->buf, buf, 3);
    qm->size = size;
    qm->reltime = tme + delay;
    self->memI[22] = (self->memI[22] + 1) % self->memI[20];

    if ((self->memI[22] + 1) % self->memI[20] == self->memI[21]) {
      return;
    }
  }
}

static void
filter_preproc_freqdelay(MidiFilter* self)
{
  int c,k;

  if (*self->cfg[3] != 0 && self->lcfg[3] == 0) {
    for (c=0; c < 16; ++c) {
      self->memF[c] = 1;
      for (k=0; k < 127; ++k) {
        self->memCM[c][k] = 0; // remember velocity of note-on
        self->memCI[c][k] = -1; // remember time of note-on
      }
    }
  }
  float newbpm = MAX(*self->cfg[2], 1.0);
  if (*self->cfg[1] && (self->available_info & NFO_BPM)) {
    newbpm = self->bpm;
  }
  if (newbpm <= 0) newbpm = 60;
}

static void
filter_postproc_freqdelay(MidiFilter* self)
{
  int i,c,k;
  const int max_delay = self->memI[20];
  const int roff = self->memI[21];
  const uint32_t n_samples = self->n_samples;
  int skipped = 0;

  float bpm = MAX(*self->cfg[2], 1.0);
  if (*self->cfg[1] && (self->available_info & NFO_BPM)) {
    bpm = self->bpm;
  }
  if (bpm <= 0) bpm = 60;
  //const float grid = RAIL((*self->cfg[3]), 1/256.0, 16.0);
  const double samples_per_beat = 60.0 / bpm * self->samplerate;

  /* add note-on/off for held notes..
   * TODO: use a preallocated stack-like structure
   */
  if (*self->cfg[3] == 0) for (c=0; c < 16; ++c) for (k=0; k < 127; ++k) {
    if (self->memCM[c][k] == 0) continue;

    if (self->memCI[c][k] >= n_samples) {
      self->memCI[c][k] -= n_samples;
      continue;
    }

    while (1) {
      self->memCM[c][k] = RAIL(rintf(self->memCM[c][k] + (*self->cfg[4])), 1, 127);
      /* enqueue note-off + note-on */
      MidiEventQueue *qm = &(self->memQ[self->memI[22]]);
      qm->buf[0] = MIDI_NOTEOFF | c;
      qm->buf[1] = k;
      qm->buf[2] = 0;
      qm->size = 3;
      qm->reltime = self->memCI[c][k];
      self->memI[22] = (self->memI[22] + 1) % self->memI[20];
      if ((self->memI[22] + 1) % self->memI[20] == self->memI[21]) {
        self->memCI[c][k] += ceil(self->memF[c] * samples_per_beat);
        break;
      }

      qm = &(self->memQ[self->memI[22]]);
      qm->buf[0] = MIDI_NOTEON | c;
      qm->buf[1] = k;
      qm->buf[2] = self->memCM[c][k];
      qm->size = 3;
      qm->reltime = self->memCI[c][k];
      self->memI[22] = (self->memI[22] + 1) % self->memI[20];
      if ((self->memI[22] + 1) % self->memI[20] == self->memI[21]) {
        self->memCI[c][k] += ceil(self->memF[c] * samples_per_beat);
        break;
      }

      self->memCI[c][k] += ceil(self->memF[c] * samples_per_beat);

      if (self->memCI[c][k] >= n_samples) {
        break;
      }
    }

    self->memCI[c][k] -= n_samples;
  }

  /* sort queue */
  int pidx = -1;
  for (i=0; i < max_delay; ++i) {
    const int off = (i + roff) % max_delay;
    if (off == self->memI[22]) {
      break;
    }
    if (self->memQ[off].size == 0) {
      continue;
    }
    if (self->memQ[off].size > 3) {
      pidx = -1;
      continue;
    }
    if (pidx < 0 || self->memQ[off].reltime >= self->memQ[pidx].reltime) {
      pidx = off;
      continue;
    }
    // swap
    MidiEventQueue tmp;
    memcpy(&tmp, &self->memQ[off], sizeof(MidiEventQueue));
    memcpy(&self->memQ[off], &self->memQ[pidx], sizeof(MidiEventQueue));
    memcpy(&self->memQ[pidx], &tmp, sizeof(MidiEventQueue));
    pidx = off;
  }

  /* dequeue delayline */
  for (i=0; i < max_delay; ++i) {
    const int off = (i + roff) % max_delay;
    if (off == self->memI[22]) break;

    if (self->memQ[off].size > 0) {
      if (self->memQ[off].reltime < n_samples) {

        if (self->memQ[off].size == 3 && (self->memQ[off].buf[0] & 0xf0) == MIDI_NOTEON) {
          const uint8_t chn = self->memQ[off].buf[0] & 0x0f;
          const uint8_t key = self->memQ[off].buf[1] & 0x7f;
          self->memCS[chn][key]++;
          if (self->memCS[chn][key] > 1) { // send a note-off first
            //self->memCS[chn][key]--;
            uint8_t buf[3];
            buf[0] = MIDI_NOTEOFF | chn;
            buf[1] = key; buf[2] = 0;
            forge_midimessage(self, self->memQ[off].reltime, buf, 3);
          }
          forge_midimessage(self, self->memQ[off].reltime, self->memQ[off].buf, self->memQ[off].size);
        }
        else if (self->memQ[off].size == 3 && (self->memQ[off].buf[0] & 0xf0) == MIDI_NOTEOFF) {
          const uint8_t chn = self->memQ[off].buf[0] & 0x0f;
          const uint8_t key = self->memQ[off].buf[1] & 0x7f;
          if (self->memCS[chn][key] > 0) {
            self->memCS[chn][key]--;
            if (self->memCS[chn][key] == 0) {
              forge_midimessage(self, self->memQ[off].reltime, self->memQ[off].buf, self->memQ[off].size);
            }
          }
        } else {
          forge_midimessage(self, self->memQ[off].reltime, self->memQ[off].buf, self->memQ[off].size);
        }

        self->memQ[off].size = 0;
        if (!skipped) self->memI[21] = (self->memI[21] + 1) % max_delay;
      } else {
        self->memQ[off].reltime -= n_samples;
        skipped = 1;
      }
    } else if (!skipped) self->memI[21] = off;
    if (off == self->memI[22]) break;
  }
}

void filter_init_freqdelay(MidiFilter* self) {
  int c,k;
  srandom ((unsigned int) time (NULL));
  self->memI[20] = MAX(self->samplerate / 8.0, 1024);
  self->memI[21] = 0; // read-pointer
  self->memI[22] = 0; // write-pointer
  self->memQ = calloc(self->memI[20], sizeof(MidiEventQueue));
  self->preproc_fn = filter_preproc_freqdelay;
  self->postproc_fn = filter_postproc_freqdelay;
  self->cleanup_fn = filter_cleanup_freqdelay;

  for (c=0; c < 16; ++c) for (k=0; k < 127; ++k) {
    self->memCS[c][k] = 0; // count note-on per key post-delay
    self->memCM[c][k] = 0; // remember velocity of note-on
    self->memCI[c][k] = -1; // remember time of note-on
  }
}

#endif
