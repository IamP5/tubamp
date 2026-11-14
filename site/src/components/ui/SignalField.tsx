"use client";

import { useEffect, useRef } from "react";
import { MQ } from "@/lib/motion";

const LINES = 64;
const POINTS = 240;

const VERT = `
attribute vec2 a;
uniform float uTime, uAspect, uLift, uDrive;
uniform vec2 uPointer;
varying float vAlpha;

float signal(float x, float t) {
  float packet = pow(max(0.0, sin(t * 0.9 + x * 2.2)), 3.0);
  float s = sin(x * 19.0 + t * 3.1) * 0.55 + sin(x * 43.0 - t * 5.3) * 0.25 + sin(x * 7.3 + t * 1.1) * 0.45;
  return s * (0.12 + packet);
}

void main() {
  float u = a.x;
  float v = a.y;
  // Each line further back is the same signal a moment earlier.
  float s = signal(u, uTime - v * 5.0);
  s = mix(s, clamp(s * 2.6, -0.62, 0.62), uDrive);
  float du = (u - uPointer.x) / 0.11;
  float touch = exp(-du * du) * uPointer.y;
  float edge = pow(max(0.0, sin(u * 3.14159)), 0.7);
  float y = s * edge * (0.34 + touch * 0.5) * (1.0 - v * 0.35);

  vec3 p = vec3((u - 0.5) * 7.0, y - 0.95, -v * 9.0) - vec3(0.0, 0.25 + uLift * 1.3, 1.5);
  float pitch = 0.16 + uLift * 0.5;
  float c = cos(pitch), sn = sin(pitch);
  p = vec3(p.x, p.y * c - p.z * sn, p.y * sn + p.z * c);
  gl_Position = vec4(p.x * 1.7 / uAspect, p.y * 1.7, 0.0, -p.z);

  float depth = pow(1.0 - v, 1.6);
  vAlpha = depth * edge * (0.16 + 0.84 * min(1.0, abs(s) * 2.4 + touch * 0.6)) * (1.0 - uLift);
}`;

const FRAG = `
precision mediump float;
uniform vec3 uColor;
varying float vAlpha;
void main() { gl_FragColor = vec4(uColor * vAlpha, vAlpha); }`;

function rgb(value: string): [number, number, number] {
  const hex = value.trim().replace("#", "");
  const n = parseInt(hex.length === 3 ? hex.replace(/./g, "$&$&") : hex, 16);
  return Number.isNaN(n) ? [0.94, 0.65, 0.23] : [(n >> 16) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

/**
 * The signal as a field of lines running into the distance. It answers to the
 * pointer (the lines swell under it) and to the scroll (the camera lifts and
 * the field fades as its box leaves the top of the viewport).
 */
export default function SignalField({ className = "", drive = 0 }: { className?: string; drive?: number }) {
  const ref = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = ref.current!;
    const gl = canvas.getContext("webgl", { antialias: true, premultipliedAlpha: true, alpha: true });
    if (!gl) return;

    const compile = (type: number, src: string) => {
      const shader = gl.createShader(type)!;
      gl.shaderSource(shader, src);
      gl.compileShader(shader);
      return shader;
    };
    const program = gl.createProgram()!;
    gl.attachShader(program, compile(gl.VERTEX_SHADER, VERT));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, FRAG));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) return;
    gl.useProgram(program);

    const data = new Float32Array(LINES * POINTS * 2);
    for (let l = 0, k = 0; l < LINES; l++) {
      for (let i = 0; i < POINTS; i++) {
        data[k++] = i / (POINTS - 1);
        data[k++] = l / (LINES - 1);
      }
    }
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
    const a = gl.getAttribLocation(program, "a");
    gl.enableVertexAttribArray(a);
    gl.vertexAttribPointer(a, 2, gl.FLOAT, false, 0, 0);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE);

    const loc = (name: string) => gl.getUniformLocation(program, name);
    const uni = { time: loc("uTime"), aspect: loc("uAspect"), lift: loc("uLift"), drive: loc("uDrive"), pointer: loc("uPointer"), color: loc("uColor") };
    gl.uniform3fv(uni.color, rgb(getComputedStyle(canvas).getPropertyValue("--accent")));
    gl.uniform1f(uni.drive, drive);

    const pointer = { x: 0.5, y: 0, tx: 0.5, ty: 0 };
    const reduced = window.matchMedia(MQ.reduced).matches;
    let frame = 0;
    let visible = false;
    let last = performance.now();
    let time = 12;

    const resize = () => {
      const dpr = Math.min(window.devicePixelRatio, 2);
      canvas.width = Math.round(canvas.clientWidth * dpr);
      canvas.height = Math.round(canvas.clientHeight * dpr);
      gl.viewport(0, 0, canvas.width, canvas.height);
      gl.uniform1f(uni.aspect, canvas.width / Math.max(1, canvas.height));
      if (reduced) draw();
    };

    const draw = () => {
      const box = canvas.getBoundingClientRect();
      const lift = Math.min(1, Math.max(0, -box.top / Math.max(1, box.height)));
      pointer.x += (pointer.tx - pointer.x) * 0.08;
      pointer.y += (pointer.ty - pointer.y) * 0.06;
      gl.uniform1f(uni.time, time);
      gl.uniform1f(uni.lift, lift);
      gl.uniform2f(uni.pointer, pointer.x, pointer.y);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      for (let l = 0; l < LINES; l++) gl.drawArrays(gl.LINE_STRIP, l * POINTS, POINTS);
    };

    const tick = (now: number) => {
      time += Math.min(0.05, (now - last) / 1000) * 0.55;
      last = now;
      draw();
      frame = requestAnimationFrame(tick);
    };

    const onMove = (e: PointerEvent) => {
      const box = canvas.getBoundingClientRect();
      pointer.tx = (e.clientX - box.left) / box.width;
      pointer.ty = e.clientY > box.top - 200 && e.clientY < box.bottom ? 1 : 0;
    };

    const ro = new ResizeObserver(resize);
    ro.observe(canvas);
    const io = new IntersectionObserver(([entry]) => {
      visible = entry.isIntersecting;
      cancelAnimationFrame(frame);
      if (visible && !reduced) {
        last = performance.now();
        frame = requestAnimationFrame(tick);
      }
    });
    io.observe(canvas);
    if (!reduced) window.addEventListener("pointermove", onMove, { passive: true });

    return () => {
      cancelAnimationFrame(frame);
      ro.disconnect();
      io.disconnect();
      window.removeEventListener("pointermove", onMove);
      gl.deleteBuffer(buffer);
      gl.deleteProgram(program);
    };
  }, [drive]);

  return <canvas ref={ref} aria-hidden className={`pointer-events-none ${className}`} />;
}
