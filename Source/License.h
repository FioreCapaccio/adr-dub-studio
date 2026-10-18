#pragma once

#include <JuceHeader.h>

/** Licenza d'uso: stesso schema di ADR Studio Suite (file .lic = base64 di {payload, signature}, firma Ed25519,
    legata al Machine ID). Prova gratuita di 30 giorni. La chiave privata resta sul computer del fornitore
    (ADR_Studio_Suite/tools/private-key.pem); qui c'e' solo la chiave pubblica. */
namespace fc::license
{
/** Funzione che la licenza deve contenere per valere per ADR Dub Studio (le licenze della Suite non la hanno). */
inline constexpr const char* kFeature = "dub_studio";
inline constexpr int kTrialDays = 30;

enum class State { Trial, TrialExpired, Licensed, Expired, Invalid };

struct Payload
{
    juce::String licenseId, issuedTo, machineId, tier, issuedAt, expiresAt;
    juce::StringArray features;
};

struct Verdict
{
    bool valid = false;
    juce::String code;      // INVALID_SIGNATURE, MACHINE_MISMATCH, WRONG_PRODUCT, EXPIRED, FILE_CORRUPTED
    juce::String reason;
    Payload payload;
};

struct Status
{
    State state = State::Trial;
    int daysLeft = 0;
    juce::String issuedTo, expiresAt, code, reason;
};

/** ID di questa macchina: SHA-256 dell'identificatore hardware (IOPlatformUUID su macOS, MachineGuid su Windows), primi 32 esadecimali maiuscoli. */
juce::String machineId();

/** Stringa firmata: le stesse chiavi, nello stesso ordine, di JSON.stringify() in ADR Studio Suite. */
juce::String canonicalPayload (const juce::var& payload);

/** Controlla una chiave (testo del file .lic): firma, macchina, prodotto, scadenza. Funzione pura (usata dai test). */
Verdict verifyKey (const juce::String& key, const juce::String& machine, int64_t nowMs);

/** Giorni rimasti di prova. */
int trialDaysLeft (int64_t installMs, int64_t nowMs);

/** Orologio "a cricchetto": se l'ora di sistema e' tornata indietro oltre la tolleranza, vale l'ultima ora vista. */
int64_t ratchetNow (int64_t clockMs, int64_t lastSeenMs);

Status current();
bool allowed (const Status&);

/** Verifica e salva la chiave (testo del .lic). */
bool importKey (const juce::String& text, juce::String& error);

juce::String describe (const Status&);        // frase per la finestra Licenza
juce::String shortLabel (const Status&);      // etichetta per il menu

bool eulaAccepted();
void acceptEula();
juce::String eulaText (const juce::String& lang);
} // namespace fc::license
