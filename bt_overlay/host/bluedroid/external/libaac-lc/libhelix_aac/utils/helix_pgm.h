/*
 * SPDX-FileCopyrightText: 2026 Zapadenec1982
 * SPDX-License-Identifier: MIT
 *
 * The Helix tables here are marked PROGMEM and read with pgm_read_byte() and
 * pgm_read_word(), the idiom of parts such as the AVR, where flash needs a load
 * instruction of its own. The ESP32 reads constant data from flash through the
 * cache with ordinary loads, so the mark is empty and the reads are plain
 * dereferences. Written for this project, not part of Helix.
 */

#ifndef HELIX_PGM_H
#define HELIX_PGM_H

#define PROGMEM
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))

#endif /* HELIX_PGM_H */
