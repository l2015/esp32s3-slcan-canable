# Changelog

## [1.1.0] - 2026-05-01

### Added
- WebUSB firmware flasher page (ESP Web Tools)
- GitHub Actions CI/CD for automated builds
- 83 kbps CAN baud rate support
- Comprehensive TWAI alert handling (BusOff recovery, error warning, error active)
- CAN ID validation (TWAI_STD_ID_MASK / TWAI_EXTD_ID_MASK)
- Retransmit control via `apply_tx_mode()`
- Reset error counters on CAN open/close
- CHANGELOG.md
- VERSION file for release management

### Fixed
- Error counters not reset on CAN close
- Missing bus status initialization on CAN open

## [1.0.0] - 2026-04-?? 

### Added
- Initial working firmware
- USB CDC with CANable VID/PID (0x16D0:0x117E)
- TinyUSB DTR compatibility fix
- Full SLCAN Classic CAN command set
- BusOff auto-recovery
- Bilingual README (English + 中文)
