# GPIO — Vigibot / RDK X5

## 1. Contexte

La config hardware Vigibot **officielle Raspberry Pi** utilise des numéros **BCM** et s'appuie sur `pigpio` (soft PWM universel, servos en µs). Sur RDK X5 :

| | Raspberry Pi | RDK X5 |
|--|--------------|--------|
| Bibliothèque | `pigpio` | WiringPi RDK (C), fallback `Hobot.GPIO` |
| Numérotation | BCM | **BOARD** (broche physique 1–40) |
| Soft PWM | Partout via pigpio | Implémenté par le helper C |
| PWM hardware | Limité | 8 canaux sur broches dédiées (X5) |

**Stratégie actuelle** : conserver la config Vigibot BCM inchangée ; traduire BCM→BOARD et générer le PWM dans un daemon WiringPi C. Le backend Python initial reste disponible comme fallback.

---

## 2. État initial : stubs no-op

Les modules fournis dans `/usr/local/vigiclient/` ne faisaient **rien** :

```javascript
// rdk-pigpio.js (origine)
Gpio.prototype.digitalWrite = function () {};
Gpio.prototype.pwmWrite = function () {};
Gpio.prototype.servoWrite = function () {};

// rdk-i2c-bus.js (origine)
openSync: function () { return { i2cWriteSync: function () { throw ... } }; }

// rdk-pca9685.js (origine)
Pca9685Driver.prototype.setPulseLength = function () {};
```

Vigibot « croyait » piloter le matériel mais aucune commande n'aboutissait.

---

## 3. Architecture du bridge GPIO

```mermaid
flowchart LR
  Node[clientrobotpi.js]
  RdkJs[rdk-pigpio.js]
  Helper[rdk-gpio-helper C]
  WiringPi[WiringPi BOARD]
  Pins[Header 40-pin]
  Node --> RdkJs
  RdkJs -->|"spawn stdin"| Helper
  Helper -->|"BCM to BOARD + soft PWM"| WiringPi
  WiringPi --> Pins
```

### Protocole stdin (texte)

| Commande | Usage |
|----------|-------|
| `out <bcm> <0\|1>` | Digital write |
| `pwm <bcm> <0-255>` | PWM moteurs/buzzer (soft PWM) |
| `servo <bcm> <pulse_us>` | Servo 500–2500 µs (soft PWM 50 Hz) |

### Fichiers

| Fichier | Rôle |
|---------|------|
| `rdk-pigpio.js` | API pigpio-like, spawn helper, `digitalWrite`, `pwmWrite`, `servoWrite` |
| `rdk-gpio-helper.c` | Daemon WiringPi, traduction BCM→BOARD, soft PWM temps réel |
| `rdk-gpio-helper.py` | Ancien backend Hobot.GPIO, fallback automatique si le binaire est absent |

Node appelle `setServos` → `servoWrite(pwm_µs)` quand `ADRESSE == -1` (pas de PCA). Idem `setPwmPwm` → `pwmWrite` pour les moteurs.

---

## 4. Table BCM → BOARD

Config Vigibot officielle Pi — traduction pour Hobot.GPIO :

| Rôle config | BCM | BOARD |
|-------------|-----|-------|
| Turret pan | 5 | 29 |
| Turret tilt | 6 | 31 |
| Gripper claw | 7 | 26 |
| Gripper tilt | 8 | 24 |
| Front left wheel (IN1, IN2) | 22, 23 | 15, 16 |
| Front right wheel | 24, 25 | 18, 22 |
| Rear left wheel | 16, 17 | 36, 11 |
| Rear right wheel | 26, 27 | 37, 13 |
| IR left | 13 | 33 |
| IR right | 9 | 21 |
| Buzzer | 4 | 7 |
| Brightness boost | 1 | 28 |
| Switch 4–7 | 18–21 | 12, 35, 38, 40 |

Validation matérielle : blink `simple_out.py` sur BOARD **37** → roue arrière droite (BCM 26).

---

## 5. Résultats par type de sortie

### Moteurs DC (`PwmPwm`) — OK

| Élément | Détail |
|---------|--------|
| Mécanisme | Soft PWM C 250 Hz, thread par pin |
| Deadzone | `INS` Vigibot : `[-100, -15, 15, 100]` (au lieu de ±1) |
| CPU | Helper natif mesuré à ~0,7 % au repos (charge active à mesurer) |
| Contrôle vitesse | Progressif (duty 0–255) |

**Problème initial** : roues qui tournaient au neutre (offset joystick + deadzone ±1 trop étroite).

### Buzzer (`Pwms`, BCM 4 → BOARD 7) — OK

Piloté via `pwmWrite` → soft PWM. Validé.

### Servos (`Servos`, pulses 500–2500 µs) — EN VALIDATION

| Tentative | Résultat |
|-----------|----------|
| Soft PWM 50 Hz + `time.sleep` | Tremblement important |
| Busy-wait `perf_counter` sur impulsion HIGH | Réduit |
| Hystérésis 15–40 µs + quantification 20 µs | Réduit |
| 1 thread `servo-engine` pour tous les servos | Réduit |
| `renice -10` sur helper | Marginal |
| Helper WiringPi C + `SCHED_FIFO` | Déployé, amélioration nette observée |
| Thread servo unique + impulsions déphasées | Déployé pour réduire jitter et appels de courant simultanés |

Le helper C utilise une horloge monotone absolue et un thread temps réel unique. Les impulsions des servos sont réparties dans la période de 20 ms afin d'éviter que plusieurs servos tirent leur courant au même instant. BCM7/BOARD26, qui ne dispose pas de PWM hardware, utilise le même moteur soft PWM C que les autres servos.

La stabilité doit encore être validée sous charge vidéo et avec les quatre servos actifs.

### IR (`Gpios`) — bridge validé

BOARD21/BCM9 est piloté directement par WiringPi. BOARD33/BCM13 appartient
à la seconde banque LSIO et reste réservée par PWM3 au démarrage ; le fork
WiringPi X5 ne sait pas la commuter correctement. Le helper natif détache
donc PWM3 puis pilote GPIO357 via l'interface GPIO du noyau. Ce contournement
ne touche à aucun GPIO moteur ou servo.

Les boutons Vigibot `COMMANDS1` 0 et 1 commandent respectivement les
illuminateurs gauche et droit.

### Switches (`Gpios`) — Non validés

Les sorties Switch 4–7 restent à tester explicitement.

---

## 6. PCA9685 — non disponible

| Constat | Détail |
|---------|--------|
| Module physique | **Absent** sur le robot |
| Scan I2C | Aucun `0x40` ou `0x70` sur bus 0, 2, 3, 4, 5, 6, 7, 8 |
| `/dev/i2c-1` | **Inexistant** (I2C1 multiplexé en PWM3) |
| `rdk-i2c-bus.js` | Stub no-op |
| `clientrobotpi.js` | `I2C.openSync(1)` — bus invalide sur cette image |

Câblage header type Pi : SDA pin **3**, SCL pin **5** (doc RDK X5).

---

## 7. PWM hardware X5 (expérimental, non activé)

Correction d'une hypothèse erronée (sample X3) : sur **X5**, la fréquence PWM HW est configurable sur une plage large.

| | X3 (commentaire sample) | **X5 (réel)** |
|--|------------------------|---------------|
| Fréquence | 48 kHz – 192 MHz | **0.05 Hz – 1 MHz** |
| Canaux | 2 (pins 32, 33) | **8** (4 groupes × 2) |

### Cartographie PWM hardware (doc D-Robotics)

| Groupe | Pins BOARD | Correspondance config Pi (approx.) |
|--------|------------|-----------------------------------|
| PWM0 | 29, 31 | Turret pan/tilt (BCM 5, 6) |
| PWM1 | 37, 24 | Rear R + gripper tilt (BCM 26, 8) |
| PWM2 | 28, 27 | Boost + rear R IN2 (BCM 1, 27) |
| PWM3 | 32, 33 | (souvent enabled par défaut) |

### Avertissement d'activation

Ne pas activer PWM0/PWM1 avec un overlay personnalisé sur le robot de référence. Le test du 20 juillet 2026 a rendu l'interface Wi-Fi indisponible jusqu'au retrait de l'overlay dans `/boot/config.txt` et au redémarrage.

La cause exacte dans le pinmux n'est pas encore isolée. Le helper natif laisse donc le PWM hardware désactivé par défaut et n'exige aucun overlay.

### Usage servo (principe)

```python
import Hobot.GPIO as GPIO
GPIO.setmode(GPIO.BOARD)
p = GPIO.PWM(29, 50)  # 50 Hz
p.start(7.5)          # 1500 µs → duty = 1500/20000 * 100 = 7.5 %
```

### Multiplexage (conflits)

| Fonction 1 | Fonction 2 |
|------------|------------|
| uart3 | i2c5 |
| i2c0 | pwm2 |
| spi2 | pwm0 |
| spi2 | pwm1 |
| i2c1 | pwm3 |

Activer un PWM **désactive** la fonction multiplexée sur ces broches.

**Note** : BCM 7 (gripper claw) → BOARD **26** n'est **pas** une broche PWM HW. Réaffectation câblage ou config Vigibot nécessaire.

---

## 8. Conséquences des contournements

| Contournement | Conséquence |
|---------------|-------------|
| Bridge C persistant | Process externe à superviser, fallback Python conservé |
| Soft PWM moteurs | Temps réel best-effort, charge active à mesurer |
| Soft PWM servos | Plus déterministe que Python, validation mécanique requise |
| Config BCM + traduction | Mapping à maintenir, risque d'erreur si câblage ≠ Pi |
| Stubs I2C/PCA laissés | INA219 / PCA non fonctionnels côté Vigibot |
| Overlay PWM0/PWM1 | Conflit observé avec le Wi-Fi ; interdit dans l'installation |

---

## 9. Pistes d'amélioration

1. Mesurer le jitter du helper WiringPi C avec les quatre servos et la vidéo actifs
2. Identifier précisément le conflit pinmux/Wi-Fi avant tout nouvel essai de PWM hardware
3. Évaluer PWM3 sur BOARD 32/33 avec recâblage, sans overlay PWM0/PWM1
4. Implémenter vrai `rdk-i2c-bus.js` + `rdk-pca9685.js` si module PCA ajouté
5. Valider IR illuminators et switches
