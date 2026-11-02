"""login: our device keys must be published (and correctly self-signed, which
nio checks) right after login; after logout the device must be gone."""
TIMEOUT = 240


async def peer(ctx):
    p = await ctx.peer()

    async def devices():
        await p.refresh_devices(ctx.target)
        return dict(p.c.device_store.active_user_devices(ctx.target) and
                    {d.device_id: d for d in p.c.device_store.active_user_devices(ctx.target)} or {})

    devs = {}
    for _ in range(60 * ctx.scale):
        devs = await devices()
        if devs:
            break
        await p.sync(1000)
    ctx.check(len(devs) >= 1, f"client device visible with verified keys ({list(devs)})")
    first = set(devs)
    room = await p.create_room(invite=[ctx.target], encrypted=False, name="login-done")
    # after logout the device must disappear from the key query
    gone = False
    for _ in range(90 * ctx.scale):
        devs = await devices()
        if not (first & set(devs)):
            gone = True
            break
        await p.sync(1000)
    ctx.check(gone, "device removed after logout")
