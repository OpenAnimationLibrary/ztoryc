# OpenPose ↔ scheletro Plastic — Design

> Stato: **design** (2026-09-29, sessione con Franco). Nessuna riga di codice.
> Decisioni di Franco: **le due direzioni**, export verso l'AI e import di
> animazioni estratte da video; prima prova su un **personaggio dalle
> proporzioni umane** (Sofia o un altro, lo sceglie lui), non sui nani.

-----

## 1. Perche'

- **Export**: il passo 6a di `DESIGN_export_to_ai.md` (ControlNet per
  fotogramma) oggi deve *indovinare* la posa dal render. Con lo scheletro la
  posa si conosce esatta: «l'animatic e' la verita' a cui l'AI deve obbedire».
- **Import**: reference acting. Franco recita davanti al telefono, un
  rilevatore di pose estrae i punti, Ztoryc li trasforma in **chiavi Plastic
  modificabili a mano** — non un mocap da ripulire fotogramma per fotogramma.

## 2. Il formato comune: JSON di OpenPose

Un file per fotogramma, `{"people":[{"pose_keypoints_2d":[x,y,c, x,y,c, …]}]}`,
coordinate in pixel dell'immagine (y verso il basso), `c` = confidenza 0–1.
Lo scrivono OpenPose, DWPose (`comfyui_controlnet_aux`, gia' installato in
Anymatix) e quasi tutti gli strumenti simili.

Due disposizioni dei punti, riconosciute dal **numero** di terne:

| # | BODY_25 (OpenPose) | COCO-18 (DWPose / controlnet_aux) |
|---|---|---|
| 0 | Nose | Nose |
| 1 | Neck | Neck |
| 2–4 | RShoulder, RElbow, RWrist | idem |
| 5–7 | LShoulder, LElbow, LWrist | idem |
| 8 | **MidHip** | RHip |
| 9–11 | RHip, RKnee, RAnkle | RKnee, RAnkle, LHip |
| 12–14 | LHip, LKnee, LAnkle | LKnee, LAnkle, REye |
| 15–18 | REye, LEye, REar, LEar | LEye, REar, LEar (15–17) |
| 19–24 | LBigToe, LSmallToe, LHeel, RBigToe, RSmallToe, RHeel | — |

🔵 **Da verificare sul file vero** prima di scrivere il lettore: la
disposizione esatta che esce dal nodo DWPose installato, e con quale nodo si
salva il JSON. Non fidarsi di questa tabella senza un file davanti.

**Dentro Ztoryc i punti si chiamano per NOME** (`LShoulder`), mai per indice:
le due disposizioni diventano una tabella di traduzione e basta.

## 3. La tabella di corrispondenza — il pezzo condiviso

Per ogni personaggio: *(colonna mesh, vertice dello scheletro)* → *punto
OpenPose*. Esempio: `sub_5_mesh_7 / spalla_sx` → `LShoulder`. Si compila una
volta, con un dialogo che elenca i vertici del rig accanto ai 25 punti, e vale
per le due direzioni.

**Dove si salva: nella libreria del personaggio `<scena>.zrig`**
(`ztoriglibrary.cpp`), nuovo elemento `<openpose-map>`.
⚠️ La libreria passa a **`version="2"`** quando contiene la tabella. Le build
che conoscono solo la 1 rifiutano di leggere una 2 e quindi **non la
risalvano** (`loadLibrary`, riga ~104): senza questo scalino una build vecchia
che pubblica una posa riscriverebbe il file senza la tabella, perdendola in
silenzio (lo stesso rischio gia' visto con `.ztrack` e `castSynced`).
Una libreria senza tabella resta `version="1"`.

## 4. Import: dal video alle chiavi

### 4.1 Angoli, non posizioni
Per ogni vertice `v` mappato **il cui padre e' mappato**, la direzione del
segmento `kp(padre) → kp(v)` nel video diventa la rotazione di `v` rispetto al
padre: il canale **`ANGLE`** di `PlasticSkeletonVertexDeformation`, che e' un
delta rispetto alla posa di riposo.

```
θ_video(v)  = atan2 del segmento padre→v nel video (y invertita)
θ_rest(v)   = stesso segmento nello scheletro a riposo
ANGLE(v)    = [θ_video(v) − θ_video(padre)] − [θ_rest(v) − θ_rest(padre)]
```

Le lunghezze restano quelle del rig: il gesto si trasferisce su proporzioni
diverse. **Non si passa dalla cinematica inversa**: il codice stesso
(`plasticskeletondeformation.h`, `setSolveSuspended`) documenta due risolutori
che si contraddicono sui pin; 25 pin per fotogramma lo farebbero esplodere.

**Da decidere nel codice**: vertici intermedi non mappati (una colonna
vertebrale con tre vertici fra `MidHip` e `Neck`). Proposta v1: restano al
riposo relativo e la rotazione va tutta sul primo vertice mappato della
catena; poi, se serve, ripartita.

### 4.2 Posizione del corpo
v1: **spenta di default**. La posizione nello spazio la decide l'animatore;
dal video arrivano i gesti. Opzione: anca (`MidHip`, o il punto medio di
`RHip`/`LHip` in COCO-18) → `ROOTX`/`ROOTY` della radice, con una scala da
calibrare sul primo fotogramma.

### 4.3 Pulizia
- **Confidenza**: sotto soglia il punto non si usa; il vertice tiene la chiave
  vicina. Niente valori inventati.
- **Smussatura**: filtro che toglie il tremolio senza ritardare i movimenti
  veloci (tipo One Euro), un parametro solo esposto.
- **Frame rate**: da 30 a 25 fps. Meglio ancora: forzare 25 fps gia' nel nodo
  che carica il video in ComfyUI (🔵 verificare che l'opzione ci sia).
- **Riduzione delle chiavi**: si tengono estremi e punti di svolta, non una
  chiave per fotogramma. Quello che esce deve essere animazione modificabile.
- **Specchio** (Franco a destra, il personaggio guarda a sinistra) e
  **scambi sinistra/destra** occasionali di OpenPose: un'opzione e un
  controllo di coerenza.
- **Persona**: se nel fotogramma ce n'e' piu' d'una, si sceglie quale (la
  piu' grande, o per indice), e si segue quella.

### 4.4 Piedi piantati — dopo la v1
Dove un piede resta fermo nel video, `PIN` + `PINTX`/`PINTY` su quel vertice,
a gradini (`PIN` e' gia' pensato per «il piede d'appoggio nella camminata»).

### 4.5 Limiti da dire a chi lo usa
- Niente profondita': un braccio puntato verso la camera sembra corto e viene
  reso come un braccio abbassato. (Estensione possibile: il rapporto fra
  lunghezza vista e lunghezza vera su `DISTANCE`, come scorcio.)
- Mani e dita: nel formato ci sono, ma inaffidabili. Fuori dalla v1.
- Faccia: resta al lipsync.

## 5. Export: dallo scheletro al JSON

Per ogni fotogramma: posizione dei vertici **deformati** (lo scheletro
valutato, come lo usa gia' il render — `PlasticDeformerStorage`), portata in
pixel della camera componendo la posizione della colonna. Si scrive il JSON e,
a richiesta, l'**immagine della posa** (bastoncini colorati su nero, i colori
standard di OpenPose) per ControlNet. Piu' personaggi = piu' `people` nello
stesso file.

⚠️ **Il punto difficile e' la composizione delle posizioni**: negli shot i
personaggi stanno dentro **sottoscene** (sh110: ogni personaggio e' una mesh
che deforma una sottoscena, dentro una sottoscena del personaggio). La catena
colonna → sottoscena → colonna padre → camera va composta come fa il render.
La diagnostica `PLASTICRENDER` (`meshInTile`) mostra gia' dove cade la mesh in
pixel: e' il riferimento per verificare l'export.

## 6. Dove sta nell'interfaccia
- Strumento Plastic, sulla colonna mesh:
  **«Import pose animation (OpenPose)…»** — cartella dei JSON, intervallo di
  fotogrammi della scena, persona, soglia di confidenza, smussatura,
  riduzione chiavi, specchio.
- **«Export OpenPose…»** — intervallo, cartella, JSON e/o immagine.
- **«OpenPose map…»** — il dialogo della tabella (§ 3).
- L'import scrive chiavi come le scriverebbe l'animatore: un solo undo.

## 7. Ordine di lavoro
1. **Tabella + export.** Non toccano il rig, si verificano a occhio (la posa
   esportata sovrapposta al render). Prima su un personaggio di primo livello,
   poi dentro sottoscena.
2. **Import con i soli angoli** (§ 4.1), senza pulizia: un video breve di
   Franco, personaggio umano.
3. Confidenza, smussatura, riduzione chiavi, specchio.
4. Posizione del corpo, piedi piantati, scorcio.

## 8. Da procurare prima di scrivere codice
- **Un JSON vero** uscito da DWPose in Anymatix (un fotogramma basta), per
  fissare la disposizione dei punti (§ 2).
- **Il personaggio della prova** con i nomi dei suoi vertici.
- **Un video di prova** di pochi secondi (Franco che alza un braccio e cammina
  sul posto): il caso piu' semplice che dice se gli angoli funzionano.
