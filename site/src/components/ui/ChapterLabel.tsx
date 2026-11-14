export default function ChapterLabel({ children }: { children: React.ReactNode }) {
  return (
    <p className="flex items-center gap-2.5 text-sm text-bone-2">
      <span className="size-1.5 rounded-full bg-[var(--accent)]" aria-hidden />
      {children}
    </p>
  );
}
