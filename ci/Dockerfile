# Update the digest and dated package snapshot together, then requalify all presets.
FROM archlinux:base-devel@sha256:2acfa2683d458b7803b7aa4ee3c139d2ba6922cd84666fcb7496a3a02fcc4c83
RUN printf 'Server = https://archive.archlinux.org/repos/2026/10/20/$repo/os/$arch\n' > /etc/pacman.d/mirrorlist \
    && pacman -Syyuu --noconfirm \
    && pacman -S --needed --noconfirm cmake ninja libsndfile pipewire pipewire-audio qt6-base qt6-declarative ttf-dejavu python \
    && pacman -Scc --noconfirm \
    && useradd --create-home --uid 1000 builder \
    && install -d -o builder -g builder /src
WORKDIR /src
COPY --chown=builder:builder . .
USER builder
ENV QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software CMAKE_BUILD_PARALLEL_LEVEL=2
ENTRYPOINT ["bash", "ci/verify.sh"]
