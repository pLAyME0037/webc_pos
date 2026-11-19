#include "../../build/bundle.h"
// The version page renders WEBC_BUILD_TIME (via display/version.h.tt), which
// moved out of bundle.h so a timestamp change would not recompile serve.c.
#include "../../build/build_time.h"
#include "version.h"
#include "src/layout/header.h"
#include "src/layout/footer.h"

void render_version_page(Serve_Context *sc) {
    String_Builder *sb = &sc->body;
    render_page_header(sb, "Version", "/version");
#include "../../build/h_to_html/version.h"
    render_page_footer(sb);
}

void serve_version_page(Serve_Context *sc) {
    sc->body.count = 0;
    render_version_page(sc);
    http_render_response(sc, 200, "text/html", sb_to_sv(sc->body));
}
