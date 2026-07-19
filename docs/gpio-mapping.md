# GPIO — Vigibot / RDK X5

## 1. Contexte

La config hardware Vigibot **officielle Raspberry Pi** utilise des numéros **BCM** et s'appuie sur `pigpio` (soft PWM universel, servos en µs). Sur RDK X5 :

| | Raspberry Pi | RDK X5 |
|--|--------------|--------|
| Bibliothèque | `pigpio` | `Hobot.GPIO` |
| Numérotation | BCM | **BOARD** (broche physique 1–40) |
| Soft PWM | Partout via pigpio | **Non** (Hobot ne l'implémente pas) |
| PWM hardware | Limité | 8 canaux sur broches dédiées (X5) |

**Stratégie POC** : conserver la config Vigibot BCM inchangée ; traduire BCM→BOARD dans un daemon Python.

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
  Helper[rdk-gpio-helper.py]
  Hobot[Hobot.GPIO BOARD]
  Pins[Header 40-pin]
  Node --> RdkJs
  RdkJs -->|"spawn stdin"| Helper
  Helper -->|"BCM to BOARD"| Hobot
  Hobot --> Pins
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
| `rdk-gpio-helper.py` | Daemon persistant, traduction BCM→BOARD, soft PWM, soft servo |

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
| Mécanisme | Soft PWM 250 Hz, thread par pin |
| Deadzone | `INS` Vigibot : `[-100, -15, 15, 100]` (au lieu de ±1) |
| CPU | ~17 % charge totale |
| Contrôle vitesse | Progressif (duty 0–255) |

**Problème initial** : roues qui tournaient au neutre (offset joystick + deadzone ±1 trop étroite).

### Buzzer (`Pwms`, BCM 4 → BOARD 7) — OK

Piloté via `pwmWrite` → soft PWM. Validé.

### Servos (`Servos`, pulses 500–2500 µs) — DÉGRADÉ

| Tentative | Résultat |
|-----------|----------|
| Soft PWM 50 Hz + `time.sleep` | Tremblement important |
| Busy-wait `perf_counter` sur impulsion HIGH | Réduit |
| Hystérésis 15–40 µs + quantification 20 µs | Réduit |
| 1 thread `servo-engine` pour tous les servos | Réduit |
| `renice -10` sur helper | Marginal |

**Résultat final** : servos **fonctionnels** mais **tremblement au repos** — jugé insuffisant.

**Cause racine** : soft PWM userspace Linux (non temps-réel), concurrence avec threads moteurs 250 Hz.

### IR / Switches (`Gpios`) — Non validés en POC

Devraient fonctionner via `digitalWrite` une fois le bridge en place. Non testés explicitement.

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

## 7. PWM hardware X5 (piste recommandée pour servos)

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

### Activation

```bash
srpi-config
# 3 Interface Options → I3 Peripheral bus config
# pwm0, pwm1, pwm2 → okay
# reboot
```

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
| Bridge Python persistant | Latence stdin/stdout, process externe à superviser |
| Soft PWM moteurs | Jitter sous charge, non temps-réel |
| Soft PWM servos | Tremblement au repos, maintien imprécis |
| Config BCM + traduction | Mapping à maintenir, risque d'erreur si câblage ≠ Pi |
| Stubs I2C/PCA laissés | INA219 / PCA non fonctionnels côté Vigibot |

---

## 9. Pistes d'amélioration

1. Migrer servos (et idéalement moteurs) vers **PWM hardware X5** via `GPIO.PWM`
2. Étendre `rdk-gpio-helper.py` pour utiliser HW PWM sur pins 29/31/37/24/28/27/32/33
3. Implémenter vrai `rdk-i2c-bus.js` + `rdk-pca9685.js` si module PCA ajouté
4. Valider IR illuminators et switches
5. Documenter et tester chaque broche avec `test_all_pins.py` / multimètre
