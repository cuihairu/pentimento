import { defineConfig } from 'vitepress'

export default defineConfig({
  base: '/pentimento/',
  title: 'pentimento',
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/pentimento/favicon.svg' }],
    // G2-L1: redirect first-time Chinese-browser visitors from the root home to /zh/,
    // remembering an explicit choice in localStorage ('pentimento-lang').
    [
      'script',
      {},
      `(function(){if(location.pathname.replace(/\\/$/,'')!=='/pentimento')return;var k='pentimento-lang',s;try{s=localStorage.getItem(k)}catch(e){}if(s)return;var zh=(navigator.language||'').toLowerCase().startsWith('zh');if(zh)try{localStorage.setItem(k,'zh')}catch(e){};if(zh)location.replace('/pentimento/zh/')})();`
    ]
  ],
  description:
    'A local desktop asset manager for game art assets, centered on version-history browsing and restore.',
  srcExclude: [
    'zh/design/**',
    'zh/review/**',
    'zh/review-log.md',
    'zh/desktop-prototype-spec.md',
    'zh/integrations.md',
    'zh/performance.md',
    'zh/requirements.md'
  ],
  locales: {
    root: {
      label: 'English',
      lang: 'en-US',
      themeConfig: {
        nav: [
          { text: 'Home', link: '/' },
          { text: 'Quickstart', link: '/quickstart' },
          { text: 'Configuration', link: '/configuration' },
          { text: 'Configuration effects', link: '/configuration-effects' },
          { text: 'Architecture', link: '/architecture' },
          { text: 'Glossary', link: '/glossary' }
        ],
        sidebar: [
          {
            text: 'Docs',
            items: [
              { text: 'Quickstart', link: '/quickstart' },
              { text: 'Configuration', link: '/configuration' },
              { text: 'Configuration effects', link: '/configuration-effects' },
              { text: 'Architecture', link: '/architecture' },
              { text: 'Glossary', link: '/glossary' }
            ]
          }
        ]
      }
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      link: '/zh/',
      themeConfig: {
        nav: [
          { text: '首页', link: '/zh/' },
          { text: '快速开始', link: '/zh/quickstart' },
          { text: '配置', link: '/zh/configuration' },
          { text: '配置效果', link: '/zh/configuration-effects' },
          { text: '架构', link: '/zh/architecture' },
          { text: '术语表', link: '/zh/glossary' }
        ],
        sidebar: [
          {
            text: '文档',
            items: [
              { text: '快速开始', link: '/zh/quickstart' },
              { text: '配置', link: '/zh/configuration' },
              { text: '配置效果', link: '/zh/configuration-effects' },
              { text: '架构', link: '/zh/architecture' },
              { text: '术语表', link: '/zh/glossary' }
            ]
          }
        ]
      }
    }
  },
  themeConfig: {
    logo: '/logo.svg',
    socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/pentimento' }]
  }
})
