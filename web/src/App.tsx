import { lazy, Suspense, useEffect } from "react";
import { SoundProvider } from "./audio/sound";
import { Layout } from "./components/Layout";
import { useRoute, type Route } from "./router";
import { Home } from "./pages/Home";
import { Install } from "./pages/Install";
// heavier pages load on demand
const Architecture = lazy(() => import("./pages/Architecture").then((m) => ({ default: m.Architecture })));
const Build = lazy(() => import("./pages/Build").then((m) => ({ default: m.Build })));
const Next = lazy(() => import("./pages/Next").then((m) => ({ default: m.Next })));
import { Team } from "./pages/Team";
import { NotFound } from "./pages/NotFound";

const TITLES: Record<Route, string> = {
  "/": "Arkivector: vector search for Arkiv",
  "/install": "Install · Arkivector",
  "/architecture": "Architecture · Arkivector",
  "/build": "Build artifacts · Arkivector",
  "/next": "What's next · Arkivector",
  "/team": "Team · Arkivector",
  "404": "Page not found · Arkivector",
};

export function App() {
  const route = useRoute();
  useEffect(() => {
    document.title = TITLES[route];
  }, [route]);
  return (
    <SoundProvider>
      <Layout route={route}>
        <Suspense fallback={<p className="mx-auto max-w-5xl px-6 py-16">loading…</p>}>
        {route === "/" && <Home />}
        {route === "/install" && <Install />}
        {route === "/architecture" && <Architecture />}
        {route === "/build" && <Build />}
        {route === "/next" && <Next />}
        {route === "/team" && <Team />}
        {route === "404" && <NotFound />}
        </Suspense>
      </Layout>
    </SoundProvider>
  );
}
