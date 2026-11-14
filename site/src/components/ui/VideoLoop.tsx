"use client";

import { useEffect, useRef, useState } from "react";
import { clipSrc, type Clip } from "@/data/media";

/**
 * A muted loop of the real editor. Sources are attached only when the element
 * nears the viewport and playback stops when it leaves, so a page full of clips
 * costs nothing until each one is looked at.
 */
export default function VideoLoop({ clip, className }: { clip: Clip; className?: string }) {
  const ref = useRef<HTMLVideoElement>(null);
  const [near, setNear] = useState(false);
  const src = clipSrc(clip);

  useEffect(() => {
    const video = ref.current;
    if (!video) return;
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
    return () => io.disconnect();
  }, []);

  return (
    <video
      ref={ref}
      className={className}
      width={clip.width}
      height={clip.height}
      style={{ aspectRatio: `${clip.width} / ${clip.height}` }}
      poster={src.poster}
      aria-label={clip.alt}
      muted
      loop
      playsInline
      preload="none"
      disablePictureInPicture
    >
      {near && (
        <>
          <source src={src.webm} type="video/webm" />
          <source src={src.mp4} type="video/mp4" />
        </>
      )}
    </video>
  );
}
