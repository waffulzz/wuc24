FROM ghcr.io/wiiu-env/devkitppc:20260225

COPY --from=ghcr.io/wiiu-env/wiiupluginsystem:20260418 /artifacts $DEVKITPRO
COPY --from=ghcr.io/wiiu-env/libmocha:20260331 /artifacts $DEVKITPRO
# Toast notifications. Needs NotificationModule.wms in the Aroma environment at
# runtime; the plugin degrades to log-only if it is missing.
COPY --from=ghcr.io/wiiu-env/libnotifications:20240426 /artifacts $DEVKITPRO

WORKDIR /project
