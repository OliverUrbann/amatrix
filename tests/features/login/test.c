/* Feature: login, saved login, device keys published, logout.
 * Peer: checks our device keys are visible and verified, then invites us to
 * a room named "login-done" as the signal to log out, and checks that the
 * device is gone afterwards. */
#include "mxtest.h"

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    char *device, **rooms;
    int r;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    MXT_OK(1, "login with password");
    MXT_OK(amx_device_id(c) && *amx_device_id(c), "got a device id (%s)", amx_device_id(c));
    MXT_OK(amx_user_id(c) && !strncmp(amx_user_id(c), "@", 1) && strstr(amx_user_id(c), a.user),
           "user id %s", amx_user_id(c));
    device = mxt_dup(amx_device_id(c));

    rooms = amx_joined_rooms(c);
    MXT_OK(rooms && !rooms[0], "no joined rooms yet");
    amx_free_strings(rooms);
    MXT_OK(amx_sync(c, 0) == 0, "first sync");

    /* saved login: same device without password */
    amx_client_free(c);
    c = amx_client_new(a.server, a.store);
    amx_set_event_handler(c, mxt_record, NULL);
    r = amx_login(c, a.user, "wrong-password-not-used");
    MXT_OK(r == 0 && !strcmp(amx_device_id(c), device), "saved login reuses device %s", device);

    /* wait for the peer's signal, then log out */
    MXT_OK(mxt_sync_until(c, mxt_have_invite, NULL, 120), "peer signalled (invite)");
    MXT_OK(amx_logout(c) == 0, "logout");
    MXT_OK(amx_sync(c, 0) != 0, "sync fails after logout");
    amx_client_free(c);

    /* the store no longer holds a login: the wrong password must fail now */
    c = amx_client_new(a.server, a.store);
    MXT_OK(amx_login(c, a.user, "wrong-password") != 0, "wrong password refused after logout (%s)", amx_error(c));
    MXT_OK(amx_login(c, a.user, a.password) == 0 && strcmp(amx_device_id(c), device),
           "new login gives a new device (%s)", amx_device_id(c));
    amx_client_free(c);
    free(device);
    amx_cleanup();
    return mxt_done();
}
