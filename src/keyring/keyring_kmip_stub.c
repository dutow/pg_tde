/*
 * Stub KMIP keyring provider used on platforms where libkmip can't be built
 * (currently: Windows/MSVC — libkmip uses GCC statement expressions,
 * collides with combaseapi.h, and contains CP-1252 source).
 *
 * InstallKmipKeyring() is a no-op so that adding a KMIP key provider
 * fails at use time ("unknown key provider type") rather than at link time.
 */

#include "postgres.h"

#include "keyring/keyring_kmip.h"

void
InstallKmipKeyring(void)
{
}
