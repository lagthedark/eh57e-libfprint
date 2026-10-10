# Driver Linux per il sensore EH57E (libfprint)

Login con impronta digitale su Linux per il sensore **Egis Technology /
LighTuning EH57E** (`USB 1c7a:057e`), montato su alcuni laptop recenti. Il
produttore fornisce driver solo per Windows; questo progetto fa
reverse-engineering del protocollo USB e del motore di matching del produttore
per offrire **login con impronta su Linux (GNOME / fprintd)**.

**In breve — a me funziona.** Lo uso ogni giorno per il login GNOME su Fedora
Bluefin (immutabile): l'enroll completa e il lock screen si sblocca alla
**prima pressione**, in circa un secondo. Tutto il necessario per lo stesso
sensore è in questo repository. Se hai questo sensore e funziona anche a te,
tienilo e miglioralo — patch e pulizie sono benvenute. È un progetto
hobbistico di interoperabilità: nessuna garanzia.

> **Stato / ambito: SOLO LIVELLO "CONVENIENZA".**
> Il sensore è un semplice sensore d'immagine: **niente match-on-chip, niente
> rilevamento di vitalità, e l'immagine dell'impronta viaggia in chiaro su USB**.
> Questo driver quindi *non* è un confine di sicurezza — è una funzione di
> comodità (come Windows Hello), e **una password deve restare sempre come
> fallback**. **Non** è idoneo ad alcuna certificazione biometrica.

---

## Funziona sul tuo dispositivo?

Hai quasi certamente questo sensore se `lsusb` mostra `1c7a:057e`. Il driver
punta a quel singolo sensore; altre parti Egis sono fuori ambito.

Comportamento attuale su Fedora Bluefin + GNOME + fprintd:

- **Enroll** (Impostazioni → Utenti → Impronta digitale): completa i 5 stadi,
  con messaggi a schermo quando una pressione è debole o il dito viene alzato
  troppo presto.
- **Lock screen / login**: sblocca alla **prima pressione** in ~1 s.
- **Dito sbagliato**: messaggio pulito *"impossibile abbinare"* e ri-prompt —
  nessun blocco.
- **Completamente reversibile**: `undeploy-fprintd.sh` ripristina l'fprintd di
  sistema.

---

## Hardware

| | |
|---|---|
| Vendor / prodotto | Egis Technology EH57E (`1c7a:057e`) |
| Sensore | sensore d'immagine in scala di grigi 70×57 px |
| Protocollo | USB vendor puro (control transfer + bulk); niente match-on-chip |
| Immagini | inviate in chiaro su USB |

---

## Architettura

- **Sottoclasse pura di `FpDevice`** — non `FpImageDevice`, e **nessuna patch al
  core** di libfprint. I frame EH57E sono 70×57 e non contengono minuzie NBIS,
  quindi il percorso immagine standard è inutilizzabile; enroll/verify sono
  implementati direttamente.
- **Trasporto USB + catena di pacchetti init/calibrazione + macchina a stati di
  rilevamento del tocco** vivono nel driver (`src/egis057e.c`).
- **Matcher** (`src/egis057e-matcher.c`): reimplementazione fedele del motore
  del produttore (`EgisTouchFPEngine057E.dll`), ottenuta per reverse-engineering
  (tabelle/blob estratti via signature scan). **Non** è la ricostruzione EH575.
- **Rilevamento di presenza** tramite un baseline di sfondo "caldo" per-pixel,
  seminato a ogni azione dal frame di calibrazione (sensore vuoto, `0x72`), così
  il rilevamento è coverage-based fin dal primo frame live.
  Vedi `docs/analysis/eh57e-presence-baseline.md`.

## Come funziona il rilevamento (breve)

Il sensore non ha un bit "dito" utilizzabile, quindi il contatto è dedotto dalle
immagini:

1. Ogni azione esegue la catena di init del produttore, che legge un **frame di
   calibrazione a sensore vuoto** (`0x72`). Quel frame diventa lo sfondo
   per-pixel.
2. Un frame live viene confrontato pixel per pixel con lo sfondo. La frazione di
   pixel che deviano oltre una soglia adattiva è la **coverage**.
3. Coverage ≥ 12 % ⇒ dito presente; ≤ 10 % ⇒ assente; in mezzo è isteresi.
4. Due frame "present" consecutivi avviano un **settle**, che sceglie il frame
   migliore; quel frame diventa uno stadio di enroll o una sonda di verify.
5. Il verify riporta il match alla prima sonda corrispondente; una pressione
   sbagliata viene scartata subito con un pulito *"impossibile abbinare"*.

Questo design conta perché lo stato di riposo del sensore è rumoroso e bimodale
(i frame vuoti possono sembrare quasi un dito con soglie assolute ingenue), e
perché lo sfondo viene azzerato a ogni apertura del dispositivo — seminarlo dal
frame di calibrazione elimina la "finestra cieca" in cui una pressione iniziale
veniva silenziosamente persa.

## Struttura del repository

```
src/                      sorgenti del driver (da copiare in libfprint)
  egis057e.c / .h
  egis057e-matcher.c / .h
tools/                    tool CLI + Makefile (da copiare in libfprint/tools)
  egis057e_enroll.c       CLI enroll / verify / monitor / capture
  egis057e_capture.c
  egis057e-match-test.c
scripts/                  helper operativi + deploy
  deploy-fprintd.sh       installa la lib per l'fprintd di sistema (reversibile)
  undeploy-fprintd.sh     ripristino
  fprintd-debug-on.sh     abilita il debug del driver nel journal di fprintd
  eh57e.sh / eh57e-debug.sh / eh57e-cleanup.sh / reset-sensor.sh / setup-access.sh
docs/analysis/            note di reverse-engineering + design
docs/engine/              artefatti di estrazione tabelle (nessun binario vendor)
AGENTS.md                 guida per agenti di coding / contributori
README_it.md              questa traduzione italiana del README
```

## Build

Serve il sorgente libfprint **1.94.100** (corrisponde all'SRPM Fedora;
ABI-compatibile con fprintd 1.94.x). È un driver a livello di sorgente, non una
patch.

1. Prendi l'albero libfprint alla 1.94.100 e sovrapponi i sorgenti:
   ```sh
   cp src/egis057e.c src/egis057e.h src/egis057e-matcher.c src/egis057e-matcher.h \
      <libfprint>/libfprint/drivers/
   cp tools/egis057e_*.c tools/Makefile <libfprint>/tools/
   ```
2. Registra il driver. In `<libfprint>/libfprint/meson.build` aggiungi una entry
   (la mappa driver è indicizzata sul numero di modello del sensore):
   ```meson
   'egis057e' : files('drivers/egis057e.c', 'drivers/egis057e-matcher.c'),
   ```
   e in `<libfprint>/meson.build` (la mappa delle opzioni driver):
   ```meson
   'egis057e': {},
   ```
3. Compila:
   ```sh
   meson setup build -Ddrivers=egis057e
   ninja -C build
   make -C tools
   ```
   Verifica della registrazione: `1c7a:057e | Egis Technology EH57E Touch Fingerprint Sensor`.

## Deploy sull'fprintd di sistema (login GNOME)

Su Fedora immutabile (Bluefin / Silverblue) **non si scrive in `/usr`**.
Lo script incluso installa la libreria sotto `/opt`, aggiunge un drop-in
systemd (`LD_LIBRARY_PATH`) e un'etichetta SELinux `lib_t`; è completamente
reversibile.

```sh
sudo bash scripts/deploy-fprintd.sh
# poi: Impostazioni -> Utenti -> Impronta digitale (enroll), blocca e prova
sudo bash scripts/undeploy-fprintd.sh   # per tornare all'fprintd di sistema
```

## Uso della CLI in sviluppo

```sh
bash scripts/eh57e.sh monitor 6        # loop di verify continuo, dito 6 = indice destro
bash scripts/eh57e-debug.sh verify 6   # debug verboso del driver -> hwtest/verify-debug.log
bash scripts/eh57e-cleanup.sh          # libera un device bloccato (killa residui + USB reset)
```

Log verbosi del driver durante l'uso normale di fprintd (temporaneo):

```sh
sudo bash scripts/fprintd-debug-on.sh
sudo systemctl restart fprintd
# riproduci, poi:
sudo journalctl -u fprintd --since "3 min ago" --no-pager | grep -E "EGIS057E|Verify|Enroll"
```

## Limiti e onestà

- **Solo livello convenienza** (vedi in alto). Niente vitalità; un'immagine
  latente sollevata potrebbe essere riprodotta. Tieni sempre un fallback password.
- Il matcher è una reimplementazione di un motore proprietario, fatta per
  interoperabilità; la separazione end-to-end è **preliminare** (calibrata su
  poche dita). Considera il tuning di falsi accetta/rifiuta come in progress.
- **I binari del produttore NON sono inclusi.** `EgisTouchFP057E.dll`,
  `EgisTouchFPEngine057E.dll`, `EgisTouchFPSensor057E.dll` e il `.inf` provengono
  dal Windows Update Catalog e **non sono ridistribuibili**; sono usati solo in
  locale per estrarre tabelle native.
- Poiché reimplementa un matcher proprietario, questo driver **non è
  proponibile upstream a libfprint così com'è**. Una futura versione ripulita
  (es. con un matcher aperto standard, se i frame sono adattabili) sarebbe la
  strada verso l'upstream.
- Testato su una sola macchina e un solo sensore; aspettati spigoli altrove.

## Contribuire

Issue, report di test e patch sono benvenuti. Vedi `AGENTS.md` per il flusso di
build e debug, i vincoli tassativi e le insidie note. I report sono più utili se
includono i log marcati `EGIS057E` da `scripts/fprintd-debug-on.sh`.

## Licenza

I sorgenti del driver sono marcati `SPDX-License-Identifier: 0BSD` (vedi
`LICENSE`). Le note di reverse-engineering e il tooling sono forniti per
interoperabilità e uso personale.
