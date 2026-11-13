/* NFC "tap to open": the badge acts as a Type 2 tag holding one URL. */
#ifndef BADGE_NFC_H_
#define BADGE_NFC_H_

/* Returns 0 on success, a negative errno otherwise (the badge works without NFC). */
int nfc_start(void);

#endif /* BADGE_NFC_H_ */
