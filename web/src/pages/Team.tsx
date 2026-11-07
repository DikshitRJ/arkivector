import type { ReactNode } from "react";
import { Page, REPO } from "../components/Layout";
import { Button, Note } from "../components/ui";

const FIELDS: { k: string; v: ReactNode }[] = [
  { k: "name", v: <span className="font-display text-[26px] sm:text-[30px] font-bold leading-none">Dikshit Rishi Jain</span> },
  { k: "from", v: "NITK Surathkal" },
  {
    k: "email",
    v: (
      <a className="ink-link" href="mailto:dikshitrjain@gmail.com">
        dikshitrjain@gmail.com
      </a>
    ),
  },
  {
    k: "code",
    v: (
      <a className="ink-link" href={REPO} target="_blank" rel="noreferrer">
        github.com/DikshitRJ/arkivector
      </a>
    ),
  },
];

export function Team() {
  return (
    <Page className="py-10 sm:py-14">
      <h1 className="text-[38px] sm:text-[50px]">
        <span className="bg-paper">Team</span>
      </h1>
      <p className="mt-3 mb-10 max-w-[60ch] text-[16px] bg-paper">One person built Arkivector for ETHGlobal Mumbai 2026.</p>

      <div className="relative max-w-2xl">
        {/* index card */}
        <div className="sheet shadow-[5px_5px_0_#000]">
          <div className="border-b-2 border-arkiv-orange px-5 py-2 text-[12.5px] flex justify-between">
            <span>contributor card</span>
            <span>no. 1 of 1</span>
          </div>
          <dl>
            {FIELDS.map((f) => (
              <div key={f.k} className="grid grid-cols-[4.5rem_minmax(0,1fr)] sm:grid-cols-[6rem_minmax(0,1fr)] border-b border-dashed border-arkiv-blue last:border-b-0">
                <dt className="px-5 py-3 text-[12.5px] self-center">{f.k}</dt>
                <dd className="px-3 py-3 self-center min-w-0 break-words">{f.v}</dd>
              </div>
            ))}
          </dl>
          <div className="px-5 pb-5 pt-2 flex items-end justify-between gap-4">
            <p className="font-hand text-arkiv-blue text-[34px] leading-none -rotate-3">Dikshit R. Jain</p>
            <p className="text-[12px] text-right">signed, Mumbai, 2026</p>
          </div>
        </div>

        {/* rubber stamp */}
        <div
          aria-label="ETHGlobal Mumbai 2026"
          className="absolute -top-6 right-3 sm:-right-10 rotate-[8deg] border-[3px] border-arkiv-orange bg-paper px-3 py-1.5 text-arkiv-orange font-display font-bold text-[14px] sm:text-[16px] leading-tight text-center shadow-[0_0_0_3px_#fff,0_0_0_4.5px_#fe7445]"
        >
          ETHGlobal
          <br />
          Mumbai 2026
        </div>
      </div>

      <div className="mt-12 grid sm:grid-cols-[minmax(0,1fr)_auto] gap-6 items-center max-w-2xl">
        <Note>questions, bugs, or ideas? the email above works, so do GitHub issues.</Note>
        <Button href={`${REPO}/issues`} external>
          Open an issue
        </Button>
      </div>
    </Page>
  );
}
