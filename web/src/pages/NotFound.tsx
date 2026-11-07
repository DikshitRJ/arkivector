import { Page } from "../components/Layout";
import { Button, Note } from "../components/ui";
import { BLUE, ORANGE } from "../ink/colors";

/** A torn-out notebook page: the ring holes are still there, the page is not. */
export function NotFound() {
  // torn edge: a zig-zag across the sheet
  const tear: string[] = [];
  for (let i = 0; i <= 24; i++) tear.push(`${i ? "L" : "M"}${10 + i * 15} ${118 + (i % 2 ? -7 : 6) + ((i * 37) % 5)}`);
  return (
    <Page className="py-14 sm:py-20">
      <div className="grid md:grid-cols-[minmax(0,1fr)_minmax(0,1fr)] gap-10 items-center">
        <svg viewBox="0 0 380 260" className="w-full max-w-md h-auto" role="img" aria-label="A notebook with a page torn out">
          <rect x="10" y="10" width="360" height="240" fill="#fff" stroke="#000" strokeWidth="2" />
          {[40, 90, 140, 190, 230].map((y) => (
            <circle key={y} cx="30" cy={y} r="7" fill="#fff" stroke="#000" strokeWidth="1.6" />
          ))}
          <path d="M52 10 V250" stroke={ORANGE} strokeWidth="1.6" />
          {/* what is left of the page above the tear */}
          {[34, 58, 82, 106].map((y) => (
            <path key={y} d={`M60 ${y} H360`} stroke="#000" strokeWidth="0.8" strokeDasharray="1 4" />
          ))}
          <path d={tear.join(" ") + " L370 10 L10 10 Z"} fill="none" stroke="#000" strokeWidth="1.6" strokeLinejoin="round" />
          {/* below the tear: empty, hatched */}
          <defs>
            <pattern id="nf-hatch" width="7" height="7" patternUnits="userSpaceOnUse">
              <path d="M-1 8 L8 -1" stroke="#000" strokeWidth="0.8" />
            </pattern>
          </defs>
          <path d={tear.join(" ") + " L370 250 L10 250 Z"} fill="url(#nf-hatch)" />
          <text x="200" y="196" textAnchor="middle" fontSize="54" fontWeight="700" fill="#fff" stroke="#000" strokeWidth="1.6" style={{ fontFamily: "var(--font-display)" }}>
            404
          </text>
          <text x="62" y="76" fontSize="22" fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
            this page was here, honest
          </text>
        </svg>
        <div>
          <h1 className="text-[36px] sm:text-[46px]">
            <span className="bg-paper">Page not found</span>
          </h1>
          <p className="mt-3 max-w-[48ch] bg-paper">
            Nothing lives at this address. If you followed a link from somewhere, it may point to an older version of the site.
          </p>
          <div className="mt-6 flex flex-wrap gap-4">
            <Button href="/">Back to the start</Button>
            <Button href="/install" variant="outline">
              Install
            </Button>
          </div>
          <Note className="mt-6 -rotate-2">unlike Arkiv entities, this one expired without a block number.</Note>
        </div>
      </div>
    </Page>
  );
}
