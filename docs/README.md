# Documentation POC — Vigibot sur RDK X5

Documentation du proof-of-concept d'intégration d'un robot **RDK X5** (D-Robotics) avec la stack **Vigibot**, initialement conçue pour Raspberry Pi.

## Index

| Document | Contenu |
|----------|---------|
| [poc-vigibot-rdk-x5.md](./poc-vigibot-rdk-x5.md) | Rapport POC complet (synthèse, architecture, tableau récap) |
| [video-encoding.md](./video-encoding.md) | Encodage H.264 : tentatives HW, solution SW libx264 |
| [yolo-source.md](./yolo-source.md) | 2ᵉ source vidéo avec overlay YOLO (BPU) |
| [gpio-mapping.md](./gpio-mapping.md) | GPIO, PWM, servos, mapping BCM→BOARD |
| [known-issues.md](./known-issues.md) | Problèmes connus, workarounds, runbook |

## Plateforme cible

| Élément | Détail |
|---------|--------|
| Carte | RDK X5 (aarch64, Ubuntu 22.04.5 LTS, kernel 6.1.83) |
| Caméra | IMX219 (CSI, mipi rx csi0) |
| Client Vigibot | `/usr/local/vigiclient/` (`clientrobotpi.js`, Node.js) |
| Config hardware | Identique à la config officielle Raspberry Pi (numéros BCM) |
| Runtime IA | BPU, `hobot_dnn.pyeasy_dnn`, `libpostprocess.so` |

## Chemins clés sur la carte

```
/usr/local/vigiclient/
├── clientrobotpi.js          # Client principal Vigibot
├── sys.json                  # Ports, CMDDIFFUSION, adresses I2C
├── robot.json                # Config hardware (souvent poussée par le serveur)
├── vigi-encode-rdk.sh/.py    # Encodeur vidéo source 0
├── vigi-encode-yolo.sh/.py   # Encodeur vidéo source 1 (YOLO)
├── rdk-pigpio.js             # Wrapper GPIO (helper natif, fallback Python)
├── rdk-gpio-helper.c         # Daemon WiringPi C (BCM→BOARD, PWM, servo)
├── rdk-gpio-helper.py        # Ancien daemon Hobot.GPIO de secours
├── rdk-i2c-bus.js            # Stub I2C (no-op à l'origine)
└── rdk-pca9685.js            # Stub PCA (no-op à l'origine)
```

## État du POC (résumé)

| Volet | État | Solution retenue |
|-------|------|------------------|
| Vidéo source 0 (H.264) | OK | libx264 software (ffmpeg) |
| Vidéo source 1 (YOLO) | OK | Pipeline Python + BPU, stream-first |
| Moteurs DC | OK | Soft PWM WiringPi C 250 Hz + deadzone ±15 |
| Buzzer | OK | Soft PWM via bridge WiringPi C |
| Servos | En validation | Soft PWM C temps réel à 50 Hz |
| Encodeur H.264 matériel | Abandonné | Incompatible décodeur navigateur |
| PCA9685 | Non disponible | Pas de module physique sur le robot |
| PWM hardware X5 (servos) | À faire | 8 canaux, 50 Hz via `srpi-config` |

## Dépôt source

Ce dossier fait partie du dépôt **vigibot-rdk-x5** (dépôt GitHub dédié à l'intégration Vigibot sur RDK X5).

- **README principal** : [../README.md](../README.md) (installation, architecture, exploitation)
- **SDK Hobot** (caméra, BPU) : dépôt séparé [x5-hobot-spdev](https://github.com/horizon/x5-hobot-spdev)
