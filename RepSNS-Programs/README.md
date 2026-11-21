# RepSNS Program Builder

Program Builder is the browser app in the [RepSNS hackathon project](../README.md). Explore a starter workout, compare training days, or ask the coach to adapt a plan. The library includes the RepSNS Master Plan and a seated upper-body plan for wheelchair users. [Open the public app](https://repsns-programs.haase-indust-0781.chatgpt.site/).

## Try a plan

1. Open a starter plan and select a training day to see its exercises. The charts use the six bundled example sessions.
2. Click the **star beside the plan title** to make it your master plan. Starring another plan moves the selection. The choice is saved for this browser.
3. Open the [coach](https://repsns-programs.haase-indust-0781.chatgpt.site/coach) and ask which exercises are scheduled on the first day. The coach uses the current master plan by default.

## Run locally

Use Node.js 24 or newer. The app uses built-in Node modules, including SQLite. From the repository root, run

```sh
cd RepSNS-Programs
npm run dev
```

Open [the local preview](http://127.0.0.1:8770/). Starter plans, charts, and library actions work without an API key. The preview saves your plans in `.local/preview.sqlite`. Set `REPSNS_PREVIEW_PORT` in your environment to use a different port.

| Command | What it does |
| --- | --- |
| `npm run dev` | Builds the app and starts the preview. Restart after editing source files. |
| `npm run check` | Checks page and plan routes, concurrent changes, browser isolation, and coach actions. Claude responses are simulated, so the checks need no key. |
| `npm run build` | Bundles the pages, sample data, and server code into `dist/server/index.js` for hosting. |

## Enable AI and publish your own copy

Plan generation and coach replies use Anthropic. For local use, set `ANTHROPIC_API_KEY` in the environment of the process that starts the preview. For GPT Sites, store the key as a **runtime secret** on your own site and redeploy. Keep its value out of source files and browser code.

Build from the `RepSNS-Programs` folder. The app expects a D1 database bound as `DB` and the schema in [the SQL migration](drizzle/0000_device_plans.sql). The [hosting configuration](.openai/hosting.json) declares the binding. Associate your copy with your own GPT Sites project when publishing.

The default model is `claude-sonnet-5-5`. Set `ANTHROPIC_MODEL` to choose another compatible model available to your account. `ANTHROPIC_EFFORT` controls effort for plan generation and defaults to `medium`.

## Where data goes

Saved plans are associated with the browser that created them. The hosted app stores plans in its database. Coach history and attachments persist in browser storage. When you send a coach message, Anthropic receives the conversation and any included attachments, along with the current master plan, saved-plan summaries, and example sessions.

## Source guide

| Folder | Contents |
| --- | --- |
| [web](web/) | Plan library, charts, and coach interface |
| [data](data/) | Starter workouts, example sessions, and bundled prompts |
| [worker](worker/) | HTTP routes, plan storage, and server-side Anthropic calls |
| [drizzle](drizzle/) | Database migration |
| [scripts](scripts/) | Build, preview, and automated checks |
