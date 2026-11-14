/** The small mono index a chapter opens with: "03 Chain". */
export default function ChapterLabel({ index, children }: { index: string; children: React.ReactNode }) {
  return (
    <p className="label flex items-center gap-3">
      <span className="text-[var(--accent)]">{index}</span>
      <span className="h-px w-8 bg-line-strong" aria-hidden />
      <span>{children}</span>
    </p>
  );
}
