# Driver Linux per il sensore EH57E (libfprint)

Questo è un piccolo driver [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
per il sensore di impronte del mio **Samsung Galaxy Book Pro 360**, un
**Egis / LighTuning EH57E** (`USB 1c7a:057e`). Egis fornisce solo driver per
Windows, quindi il progetto fa reverse-engineering del protocollo USB e del
motore di matching per far funzionare il login con impronta su Linux. Lo uso su
**Fedora Bluefin** con GNOME. L'enroll funziona e lo sblocco di solito avviene
alla prima pressione, anche se non sempre; quando aggancia, ci mette meno di
tre secondi.

Prima di tutto: **è una funzione di comodità, non di sicurezza.** Il sensore si
limita a inviare un'immagine su USB — niente match-on-chip e niente rilevamento
di vitalità — quindi è della stessa categoria di Windows Hello, non di un
lucchetto. Tieni sempre la password come fallback.

## Che cos'è davvero

I frame dell'EH57E sono 70×57 pixel e non contengono minuzie NBIS, quindi il
percorso immagine normale di libfprint qui non è utilizzabile. Il driver è
quindi una semplice sottoclasse di `FpDevice` — nessuna patch al core — che
parla direttamente il protocollo USB del produttore e implementa da sé enroll e
verify. Il matcher è una reimplementazione del motore del produttore, ottenuta
per reverse-engineering dal driver Windows; il rilevamento del tocco costruisce
uno sfondo per-pixel dal frame di calibrazione a sensore vuoto dello stesso
sensore e guarda quanto dell'immagine se ne discosta. Il resto delle note di
reverse-engineering e di design sta in `docs/`.

Funziona, ma prendilo come un progetto hobbistico che per ora va bene su una
macchina sola.

## Il dispositivo

L'EH57E è il sensore della mia macchina, un **Samsung Galaxy Book Pro 360**
(2021; i modelli da 13,3" e 15,6" sono NP930QDB e NP950QDB). La stessa parte
`1c7a:057e` compare anche sul **Galaxy Book Pro** non-360 dello stesso anno
(modelli 935XDB e 950XDB), e non ho trovato portatili non-Samsung che lo usino.
Altre parti Egis — per esempio `1c7a:0575` (EH575) — sono sensori diversi e
fuori ambito. Se `lsusb` mostra `1c7a:057e`, questo driver è per te.

## Compilazione

È un driver a livello di sorgente, non una patch. Serve il sorgente libfprint
alla versione **1.94.100** (quella di Fedora; ABI-compatibile con fprintd
1.94.x). Copia il driver in un albero libfprint, registralo e compila:

```sh
cp src/egis057e.c src/egis057e.h src/egis057e-matcher.c src/egis057e-matcher.h \
   <libfprint>/libfprint/drivers/
cp tools/egis057e_*.c tools/Makefile <libfprint>/tools/
```

In `<libfprint>/libfprint/meson.build`, aggiungi il driver insieme agli altri:

```meson
'egis057e' : files('drivers/egis057e.c', 'drivers/egis057e-matcher.c'),
```

e in `<libfprint>/meson.build`:

```meson
'egis057e': {},
```

poi

```sh
meson setup build -Ddrivers=egis057e
ninja -C build
make -C tools
```

Alla fine dovresti avere registrato `1c7a:057e | Egis Technology EH57E Touch
Fingerprint Sensor`.

## Installazione per il login GNOME

Una volta compilato, installa la libfprint risultante dove l'fprintd di sistema
la trova, poi iscrivi l'impronta in Impostazioni -> Utenti -> Impronta digitale e
blocca lo schermo per provare.

Se sei su un'immagine **immutabile** come me (Fedora Bluefin / Silverblue) non
puoi scrivere in `/usr`, quindi servono un paio di passaggi in più. Ci pensa lo
script incluso: installa la libreria sotto `/opt`, indica a fprintd di usarla
con un drop-in systemd e le mette l'etichetta SELinux. È reversibile:

```sh
sudo bash scripts/deploy-fprintd.sh
# poi: Impostazioni -> Utenti -> Impronta digitale, e blocca lo schermo per provare
sudo bash scripts/undeploy-fprintd.sh   # torna all'fprintd di sistema
```

## Provarlo senza toccare il sistema

C'è una piccola CLI, utile mentre si smanetta:

```sh
bash scripts/eh57e.sh monitor 6        # loop di verify, 6 = indice destro
bash scripts/eh57e-debug.sh verify 6   # log verboso del driver
bash scripts/eh57e-cleanup.sh          # sblocca il lettore se si impalla
```

Se ti serve il log del driver mentre usi il login GNOME normale, c'è
`scripts/fprintd-debug-on.sh` (temporaneo; vedi `AGENTS.md`).

## Limiti, onestamente

Il matcher è una reimplementazione di un motore proprietario, scritta per
interoperabilità, ed è calibrato su poche dita: aspettati ogni tanto un falso
accetta o un falso rifiuta, e considera il tuning come un lavoro in corso. I
binari del produttore non fanno parte del progetto — sono stati usati solo in
locale, per studiare il protocollo. Poiché il matcher è ricostruito e non
originale, questo driver non può essere proposto upstream a libfprint così
com'è; una versione basata su un matcher aperto standard sarebbe la strada. Ed è
stato provato davvero solo sul mio portatile, quindi aspettati spigoli altrove.

## Crediti

Questo progetto è stato realizzato con l'aiuto di un agente di coding IA:
**OpenCode** (https://opencode.ai, https://github.com/anomalyco/opencode)
guidato dalla configurazione **oh-my-opencode-slim**
(https://github.com/alvinunreal/oh-my-opencode-slim). Il lavoro sul protocollo
si è appoggiato molto al precedente sforzo sull'EH575,
https://github.com/cosct/libfprint-egis0575, i cui coefficienti si sono
rivelati condivisi in tutta la famiglia Egis EH5xx.

## Licenza

I sorgenti del driver sono `0BSD` (vedi `LICENSE`). Le note di
reverse-engineering e il tooling sono forniti per interoperabilità e uso
personale.
