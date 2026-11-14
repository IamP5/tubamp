/** The plain frame real product imagery sits in. */
export default function Window({ children, className = "" }: { children: React.ReactNode; className?: string }) {
  return (
    <div className={`overflow-hidden rounded-xl border border-line-strong bg-ink-1 ${className}`}>
      {children}
    </div>
  );
}
