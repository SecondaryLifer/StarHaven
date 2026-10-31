#include "common.h"
//#include "audio/public.h"
#ifdef PM3DS_AMB_DBG
#include "audio.h"
#include "audio/public.h"
#endif

typedef struct {
    /* 0x0 */ s16 flags;
    /* 0x2 */ s16 fadeState;
    /* 0x4 */ s32 fadeTime;
    /* 0x8 */ s32 soundID;
    /* 0xC */ s32 unkC;
} AmbientSoundSettings;

typedef enum AmbientSoundState {
    AMBIENCE_STATE_IDLE         = 0,
    AMBIENCE_STATE_FADE_OUT     = 1,  // fade out old sounds
    AMBIENCE_STATE_FADE_IN      = 2   // fade in new sounds
} AmbientSoundState;

typedef enum AmbientSoundFlag {
    AMBIENCE_FLAG_PLAYING       = 1,
} AmbientSoundFlag;

AmbientSoundSettings DefaultAmbientSoundData = {
    .flags = 0,
    .fadeState = 0,
    .fadeTime = 0,
    .soundID = -1,
    .unkC = -1
};

AmbientSoundSettings AmbientSoundData;

void reset_ambient_sounds(void) {
    AmbientSoundData = DefaultAmbientSoundData;
}

void update_ambient_sounds(void) {
    AmbientSoundSettings* ambientSoundState = &AmbientSoundData;
    s32 error;

#ifdef PM3DS_AMB_DBG
    {   // debug: the ambience state machine and its player (GitHub #38, #46)
        extern void pm3ds_log(int, const char*, ...);
        extern AmbienceManager* gAuAmbienceManager;
        static s32 lastState = -1, lastSound = -2, tick;
        if (ambientSoundState->fadeState != lastState || ambientSoundState->soundID != lastSound || ++tick % 120 == 0) {
            AmbiencePlayer* p = gAuAmbienceManager != nullptr ? &gAuAmbienceManager->players[0] : nullptr;
            s32 nv = 0;
            if (gAuAmbienceManager != nullptr) {
                for (s32 i = 0; i < ARRAY_COUNT(gAuAmbienceManager->globals->voices); i++) {
                    nv += gAuAmbienceManager->globals->voices[i].priority == AU_PRIORITY_MSEQ_MANAGER;
                }
            }
            pm3ds_log(2, "AMBDBG voices %d", nv);
            pm3ds_log(2, "AMBDBG state %d sound %d flags %d active %d p0 read %p mode %d play %d fade %d/%d stopped %d",
                      ambientSoundState->fadeState, ambientSoundState->soundID, ambientSoundState->flags,
                      gAuAmbienceManager != nullptr ? gAuAmbienceManager->numActivePlayers : -1,
                      p != nullptr ? (void*)p->mseqReadPos : nullptr, p != nullptr ? p->mode : -1,
                      p != nullptr ? p->playState : -1, p != nullptr ? p->fadeSettingsType : -1,
                      p != nullptr ? p->fadeSettingsTime : -1, snd_ambient_is_stopped(0));
            lastState = ambientSoundState->fadeState;
            lastSound = ambientSoundState->soundID;
        }
    }
#endif

#ifdef _3DS
    {
        // mseq_player.c, GitHub #56: 2 s after the last ambience ended, nothing may be left of it
        extern volatile s32 pm3dsAmbienceCheck;
        static s32 silentSteps;
        if (ambientSoundState->fadeState == AMBIENCE_STATE_IDLE && ambientSoundState->soundID < 0
            && !(ambientSoundState->flags & AMBIENCE_FLAG_PLAYING)) {
            if (++silentSteps >= 60) {
                silentSteps = 0;
                pm3dsAmbienceCheck = true;
            }
        } else {
            silentSteps = 0;
        }
    }
#endif

    switch (ambientSoundState->fadeState) {
        case AMBIENCE_STATE_IDLE:
            break;
        case AMBIENCE_STATE_FADE_OUT:
            if (ambientSoundState->flags & 1) {
                if (ambientSoundState->fadeTime < 250) {
                    error = snd_ambient_stop_quick(0);
                } else {
                    error = snd_ambient_stop_slow(0, ambientSoundState->fadeTime);
                }

                if (error != AU_RESULT_OK) {
                    return;
                }
            }
            ambientSoundState->fadeState = AMBIENCE_STATE_FADE_IN;
            break;
        case AMBIENCE_STATE_FADE_IN:
            if (ambientSoundState->flags & AMBIENCE_FLAG_PLAYING) {
                if (snd_ambient_is_stopped(0) != AU_RESULT_OK) {
                    return;
                }
                ambientSoundState->flags &= ~AMBIENCE_FLAG_PLAYING;
            }
            if (ambientSoundState->soundID < 0) {
                ambientSoundState->fadeState = AMBIENCE_STATE_IDLE;
            } else if (snd_load_ambient(ambientSoundState->soundID) == AU_RESULT_OK) {
                if (snd_ambient_play(0, 0) == AU_RESULT_OK) {
                    ambientSoundState->fadeState = AMBIENCE_STATE_IDLE;
                    ambientSoundState->flags |= AMBIENCE_FLAG_PLAYING;
                }
            }
            break;
    }
}

s32 play_ambient_sounds(s32 soundID, s32 fadeTime) {
    AmbientSoundSettings* state = &AmbientSoundData;

    if (!gGameStatusPtr->musicEnabled) {
        snd_ambient_stop_quick(state->soundID);
        state->flags &= ~AMBIENCE_FLAG_PLAYING;
        return 1;
    }

    if (state->soundID == soundID) {
        return 2;
    }

    state->soundID = soundID;
    state->fadeTime = fadeTime;
    state->fadeState = AMBIENCE_STATE_FADE_OUT;
    return 1;
}
