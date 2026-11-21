import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  // host: true lets a tablet/phone on the same Wi-Fi open http://<laptop-ip>:5173
  server: { host: true },
});
