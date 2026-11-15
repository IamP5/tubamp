"use client";

import { useEffect, useRef, useState } from "react";
import { clipSrc, type Clip } from "@/data/media";
import { useDict } from "@/i18n/DictProvider";

export default function VideoLoop({
  clip,
  className,
  play = true,
}: {
  clip: Clip;
  className?: string;
  play?: boolean;
}) {
  const ref = useRef<HTMLVideoElement>(null);
  const [near, setNear] = useState(false);
  const src = clipSrc(clip);
  const alt = useDict().media[clip.name];

  useEffect(() => {
    const video = ref.current;
    if (!video || !play) return;
    const io = new IntersectionObserver(
      ([entry]) => {
        if (entry.isIntersecting) {
          setNear(true);
          video.play().catch(() => {});
        } else {
          video.pause();
        }
      },
      { rootMargin: "200px 0px" },
    );
    io.observe(video);
    return () => {
      io.disconnect();
      video.pause();
    };
  }, [play]);

  return (
    <video
      ref={ref}
      className={className}
      width={clip.width}
      height={clip.height}
      style={{ aspectRatio: `${clip.width} / ${clip.height}` }}
      poster={src.poster}
      aria-label={alt}
      muted
      loop
      playsInline
      preload="none"
      disablePictureInPicture
    >
      {near && play && (
        <>
          <source src={src.webm} type="video/webm" />
          <source src={src.mp4} type="video/mp4" />
        </>
      )}
    </video>
  );
}
