import { useEffect } from "react";
import { Layout } from "./components/Layout";
import { useRoute, type Route } from "./router";
import { Home } from "./pages/Home";
import { Install } from "./pages/Install";
import { Architecture } from "./pages/Architecture";
import { Team } from "./pages/Team";

const TITLES: Record<Route, string> = {
  "/": "Arkivector: vector search for Arkiv",
  "/install": "Install · Arkivector",
  "/architecture": "Architecture · Arkivector",
  "/team": "Team · Arkivector",
};

export function App() {
  const route = useRoute();
  useEffect(() => {
    document.title = TITLES[route];
  }, [route]);
  return (
    <Layout route={route}>
      {route === "/" && <Home />}
      {route === "/install" && <Install />}
      {route === "/architecture" && <Architecture />}
      {route === "/team" && <Team />}
    </Layout>
  );
}
