/*
 * driver/usb_hid_kbd.c â€” ALOS USB HID Boot Keyboard driver
 *
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * ARCHITECTURE
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *
 * On reste volontairement sans stack USB complÃ¨te. La stratÃ©gie :
 *
 *  1. PROBE  â€” Scan des ports xHCI connectÃ©s via usb_probe / usb_xhci.
 *              Lecture PORTSC pour dÃ©tecter une connexion FS/LS/HS.
 *
 *  2. RESET  â€” Port Reset (PORTSC PR bit), attente Port Enabled (PED).
 *
 *  3. SLOT   â€” Enable Slot Command â†’ Slot ID assignÃ© par le xHC.
 *
 *  4. ADDRESS DEVICE â€” Allocation Input Context + Transfer Ring EP0,
 *                      Address Device Command (BSR=0).
 *
 *  5. GET DESCRIPTOR (Device) â€” Control transfer EP0 pour lire VID/PID
 *                               et confirmer que c'est bien un pÃ©riphÃ©rique USB.
 *
 *  6. GET DESCRIPTOR (Configuration) â€” Scan des interfaces pour trouver
 *                                      class=3 (HID) subclass=1 (boot) protocol=1 (keyboard).
 *
 *  7. SET CONFIGURATION â€” Active la configuration 1.
 *
 *  8. SET PROTOCOL (Boot) â€” HID class request : SET_PROTOCOL = 0 (Boot Protocol).
 *                           Garantit le format fixe de 8 octets.
 *
 *  9. SET IDLE (0ms) â€” Optionnel mais recommandÃ© : dÃ©sactive les rapports
 *                      rÃ©pÃ©tÃ©s quand rien ne change.
 *
 * 10. POLL â€” GET_REPORT (Interrupt IN) polling toutes les itÃ©rations shell.
 *            DÃ©code le rapport boot keyboard 8 octets.
 *            Traduit HID Usage IDs â†’ keycodes ALOS via table hid_to_alos[].
 *            Injecte via keyboard_inject_tap() + keyboard_set_external_modifiers().
 *
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * FORMAT RAPPORT HID BOOT KEYBOARD (8 octets fixes)
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *
 *  Byte 0 : Modifier byte
 *    bit 0 : LEFT CTRL    bit 4 : RIGHT CTRL
 *    bit 1 : LEFT SHIFT   bit 5 : RIGHT SHIFT
 *    bit 2 : LEFT ALT     bit 6 : RIGHT ALT
 *    bit 3 : LEFT GUI     bit 7 : RIGHT GUI
 *  Byte 1 : Reserved (toujours 0)
 *  Bytes 2-7 : Keycodes (jusqu'Ã  6 touches simultanÃ©es, 0x00 = aucune)
 *
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * CONTRAINTES ALOS
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 *
 *  - Adressage 32 bits : mmio_base <= 0xFFFFFFFF (garanti par usb_probe).
 *  - pmm_alloc() retourne des pages physiques 4K-alignÃ©es identitÃ©-mappÃ©es.
 *  - Pas d'IOMMU, pas de cache flush explicite (UC via MTRR BIOS sur xHCI MMIO).
 *  - Un seul clavier USB gÃ©rÃ© simultanÃ©ment.
 *  - Layout AZERTY : la translation HIDâ†’ALOS cible les keycodes de keyboard.h.
 */

#include "usb_hid_kbd.h"
#include "usb_probe.h"
#include "usb_xhci.h"
#include "keyboard.h"
#include "../kernel/memory/pmm.h"
#include "../kernel/lib/kprintf.h"
#include "../kernel/lib/string.h"

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * REGISTRES xHCI (offsets depuis op_base)
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

#define XHCI_USBCMD         0x00u
#define XHCI_USBSTS         0x04u
#define XHCI_DNCTRL         0x14u
#define XHCI_CRCR_LO        0x18u
#define XHCI_CRCR_HI        0x1Cu
#define XHCI_DCBAAP_LO      0x30u
#define XHCI_DCBAAP_HI      0x34u
#define XHCI_CONFIG         0x38u

/* USBCMD bits */
#define XHCI_CMD_RUN        (1u << 0)
#define XHCI_CMD_HCRST      (1u << 1)
#define XHCI_CMD_INTE       (1u << 2)

/* USBSTS bits */
#define XHCI_STS_HCH        (1u << 0)
#define XHCI_STS_CNR        (1u << 11)

/* PORTSC (depuis op_base + 0x400 + port*0x10) */
#define XHCI_PORTSC_BASE    0x400u
#define XHCI_PORTSC_STRIDE  0x10u

#define PORTSC_CCS          (1u << 0)   /* Current Connect Status */
#define PORTSC_PED          (1u << 1)   /* Port Enabled/Disabled  */
#define PORTSC_PR           (1u << 4)   /* Port Reset             */
#define PORTSC_PP           (1u << 9)   /* Port Power             */
#define PORTSC_CSC          (1u << 17)  /* Connect Status Change  */
#define PORTSC_PEC          (1u << 18)  /* Port Enable Change     */
#define PORTSC_PRC          (1u << 21)  /* Port Reset Change      */
/* W1C bits Ã  prÃ©server lors d'une Ã©criture (ne pas setter accidentellement) */
#define PORTSC_W1C_MASK     (PORTSC_CSC | PORTSC_PEC | PORTSC_PRC | \
                             (1u<<19)|(1u<<20)|(1u<<22)|(1u<<23)|(1u<<24))

/* Door Bell (depuis cap_base + DB_OFFSET, stride 4) */
/* L'offset DB Array est dans HCCPARAMS1[31:16] * 4, mais on peut aussi
   utiliser l'offset fixe 0x880 qui est standard pour la majoritÃ© des xHC.
   On lit le vrai offset depuis le registre cap. */
#define XHCI_DBOFF_REG      0x14u   /* offset depuis mmio_base (cap regs) */

/* Runtime registers (RTSOFF depuis mmio_base+0x18) */
#define XHCI_RTSOFF_REG     0x18u
#define XHCI_HCSPARAMS2_REG 0x08u
#define XHCI_HCCPARAMS1_REG 0x10u

#define EHCI_PORTSC_BASE    0x44u
#define EHCI_PORTSC_STRIDE  0x04u
#define EHCI_PORTSC_CCS     (1u << 0)
#define EHCI_PORTSC_PE      (1u << 2)
#define EHCI_PORTSC_OWNER   (1u << 12)

/* Registres OHCI (offsets depuis mmio_base). */
#define OHCI_REVISION       0x00u
#define OHCI_CONTROL        0x04u
#define OHCI_CMD_STATUS     0x08u
#define OHCI_INT_STATUS     0x0Cu
#define OHCI_INT_ENABLE     0x10u
#define OHCI_INT_DISABLE    0x14u
#define OHCI_HCCA           0x18u
#define OHCI_CTRL_HEAD_ED   0x20u
#define OHCI_CTRL_CUR_ED    0x24u
#define OHCI_PERIOD_CUR_ED  0x1Cu
#define OHCI_FM_INTERVAL    0x34u
#define OHCI_PERIODIC_START 0x40u
#define OHCI_LS_THRESHOLD   0x44u
#define OHCI_RH_DESC_A      0x48u
#define OHCI_RH_STATUS      0x50u
#define OHCI_RH_PORT_BASE   0x54u

#define OHCI_CTL_CLE        (1u << 4)
#define OHCI_CTL_PLE        (1u << 2)
#define OHCI_CTL_IR         (1u << 8)
#define OHCI_CTL_HCFS_OP    (2u << 6)
#define OHCI_CMD_HCR        (1u << 0)
#define OHCI_CMD_CLF        (1u << 1)
#define OHCI_CMD_OCR        (1u << 3)
#define OHCI_INT_WDH        (1u << 1)
#define OHCI_INT_MIE        (1u << 31)
#define OHCI_RH_STATUS_LPSC (1u << 16)
#define OHCI_RH_PORT_CCS    (1u << 0)
#define OHCI_RH_PORT_PES    (1u << 1)
#define OHCI_RH_PORT_PRS    (1u << 4)
#define OHCI_RH_PORT_PPS    (1u << 8)
#define OHCI_RH_PORT_LSDA   (1u << 9)
#define OHCI_RH_PORT_CSC    (1u << 16)
#define OHCI_RH_PORT_PESC   (1u << 17)
#define OHCI_RH_PORT_PRSC   (1u << 20)
#define OHCI_RH_PORT_W1C    (OHCI_RH_PORT_CSC | OHCI_RH_PORT_PESC | OHCI_RH_PORT_PRSC)

/* Interrupter 0 (depuis runtime_base + 0x20) */
#define IR0_IMAN            0x00u
#define IR0_IMOD            0x04u
#define IR0_ERSTSZ          0x08u
#define IR0_ERSTBA_LO       0x10u
#define IR0_ERSTBA_HI       0x14u
#define IR0_ERDP_LO         0x18u
#define IR0_ERDP_HI         0x1Cu

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * STRUCTURES xHCI (packed, 32-bit physique)
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

/* TRB gÃ©nÃ©rique (16 octets) */
typedef struct __attribute__((packed)) {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} Trb;

typedef struct __attribute__((packed)) {
    uint32_t control;
    uint32_t tailp;
    uint32_t headp;
    uint32_t nexted;
} OhciEd;

typedef struct __attribute__((packed)) {
    uint32_t control;
    uint32_t cbp;
    uint32_t nexttd;
    uint32_t be;
} OhciTd;

typedef struct __attribute__((packed)) {
    uint32_t int_table[32];
    uint16_t frame_number;
    uint16_t pad;
    uint32_t done_head;
    uint8_t reserved[120];
} OhciHcca;

#define OHCI_ED_FA(a)       ((uint32_t)((a) & 0x7Fu))
#define OHCI_ED_EN(e)       ((uint32_t)(((e) & 0x0Fu) << 7))
#define OHCI_ED_DIR_IN      (2u << 11)
#define OHCI_ED_LOW_SPEED   (1u << 13)
#define OHCI_ED_SKIP        (1u << 14)
#define OHCI_ED_MPS(m)      ((uint32_t)((m) & 0x07FFu) << 16)

#define OHCI_TD_DP_SETUP    (0u << 19)
#define OHCI_TD_DP_OUT      (1u << 19)
#define OHCI_TD_DP_IN       (2u << 19)
#define OHCI_TD_DI_NONE     (7u << 21)
#define OHCI_TD_T_DATA0     (2u << 24)
#define OHCI_TD_T_DATA1     (3u << 24)
#define OHCI_TD_CC_SHIFT    28
#define OHCI_TD_CC_NOT_ACC  (0xFu << OHCI_TD_CC_SHIFT)
#define OHCI_CC_NO_ERROR    0u
#define OHCI_CC_DATA_UNDERRUN 9u

/* TRB control field bits */
#define TRB_CYCLE           (1u << 0)
#define TRB_ENT             (1u << 1)   /* Evaluate Next TRB */
#define TRB_ISP             (1u << 2)   /* Interrupt on Short Packet */
#define TRB_NS              (1u << 3)   /* No Snoop */
#define TRB_CH              (1u << 4)   /* Chain */
#define TRB_IOC             (1u << 5)   /* Interrupt on Completion */
#define TRB_IDT             (1u << 6)   /* Immediate Data */
#define TRB_TYPE_SHIFT      10
#define TRB_TYPE(t)         ((uint32_t)(t) << TRB_TYPE_SHIFT)
#define TRB_DIR_IN          (1u << 16)

/* TRB types */
#define TRBTYPE_NORMAL      1u
#define TRBTYPE_SETUP       2u
#define TRBTYPE_DATA        3u
#define TRBTYPE_STATUS      4u
#define TRBTYPE_ISOCH       5u
#define TRBTYPE_LINK        6u
#define TRBTYPE_EVENT_DATA  7u
#define TRBTYPE_NOOP        8u
#define TRBTYPE_ENABLE_SLOT 9u
#define TRBTYPE_DISABLE_SLOT 10u
#define TRBTYPE_ADDRESS_DEV 11u
#define TRBTYPE_CONFIG_EP   12u
#define TRBTYPE_EVAL_CTX    13u
#define TRBTYPE_NOOP_CMD    23u

/* Event TRB types */
#define EVTYPE_TRANSFER     32u
#define EVTYPE_CMD_COMPLETE 33u
#define EVTYPE_PORT_STATUS  34u
#define EVTYPE_BW_REQUEST   35u
#define EVTYPE_DOORBELL     36u
#define EVTYPE_HOST_CTRL    37u
#define EVTYPE_DEV_NOTIF    38u
#define EVTYPE_MFINDEX_WRAP 39u

/* Completion codes */
#define CC_SUCCESS          1u
#define CC_DATA_BUFFER      2u
#define CC_BABBLE           3u
#define CC_USB_TRANSACTION  4u
#define CC_TRB_ERROR        5u
#define CC_STALL            6u
#define CC_SHORT_PACKET     13u

/* Slot context (entrÃ©e 0 du device context, 32 octets utilisÃ©s) */
typedef struct __attribute__((packed)) {
    uint32_t dw0;   /* Route String[19:0], Speed[23:20], MTT[25], HUB[26], CtxEntries[31:27] */
    uint32_t dw1;   /* Max Exit Latency[15:0], RH Port Number[23:16], NumPorts[31:24] */
    uint32_t dw2;   /* TT Hub Slot ID[7:0], TT Port Number[15:8], TTT[17:16], IRC[27:20] */
    uint32_t dw3;   /* USB Device Address[7:0], Slot State[31:27] */
    uint32_t rsvd[4];
} SlotCtx;

/* Endpoint context (32 octets) */
typedef struct __attribute__((packed)) {
    uint32_t dw0;   /* EP State[2:0], Mult[9:8], MaxPStreams[14:10], LSA[15], Interval[23:16] */
    uint32_t dw1;   /* CErr[2:1], EP Type[5:3], HID[7], MaxBurstSize[15:8], MaxPacketSize[31:16] */
    uint32_t tr_deq_lo;  /* TR Dequeue Pointer Lo + DCS */
    uint32_t tr_deq_hi;  /* TR Dequeue Pointer Hi */
    uint32_t dw4;   /* Average TRB Length[15:0], Max ESIT Payload[31:16] */
    uint32_t rsvd[3];
} EpCtx;

/* Device Context = Slot + 31 EP contexts (on n'en utilise que 2 : EP0 + EP1 IN) */
#define DEV_CTX_SIZE        (32u * 32u)   /* 32 entrÃ©es Ã— 32 octets */

/* Input Context = Input Control Context (32B) + Device Context */
#define INPUT_CTRL_CTX_SIZE 32u
#define INPUT_CTX_SIZE      (INPUT_CTRL_CTX_SIZE + DEV_CTX_SIZE)

/* EP types */
#define EP_TYPE_CTRL        4u
#define EP_TYPE_ISOCH_OUT   1u
#define EP_TYPE_BULK_OUT    2u
#define EP_TYPE_INT_OUT     3u
#define EP_TYPE_ISOCH_IN    5u
#define EP_TYPE_BULK_IN     6u
#define EP_TYPE_INT_IN      7u

/* Speed codes xHCI */
#define SPEED_FULL          1u
#define SPEED_LOW           2u
#define SPEED_HIGH          3u
#define SPEED_SUPER         4u

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * RING SIZES
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

#define CMD_RING_TRBS       16u     /* Command Ring : 16 TRBs (256B, dans une page) */
#define EVT_RING_TRBS       16u     /* Event Ring   : 16 TRBs */
#define EP0_RING_TRBS       8u      /* Transfer Ring EP0 */
#define EP1_RING_TRBS       8u      /* Transfer Ring EP1 IN (interrupt) */

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * Ã‰TAT GLOBAL DU DRIVER
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

typedef struct {
    int         present;        /* 1 si clavier trouvÃ© et initialisÃ©     */
    int         error;          /* 1 si erreur irrÃ©cupÃ©rable             */
    uint8_t     backend;        /* 1=xHCI, 2=OHCI                         */
    uint8_t     slot_id;        /* Slot xHCI assignÃ© (1-255)             */
    uint8_t     port_idx;       /* Index de port xHCI (0-based)          */
    uint8_t     speed;          /* Speed xHCI (SPEED_*)                  */
    uint8_t     max_slots;      /* Max Slots Enabled pour les essais     */
    uint8_t     ctx_size;       /* Taille d'un contexte xHCI (32 ou 64)  */
    uint8_t     ep1_addr;       /* Adresse USB de l'endpoint IN (0x81 typiquement) */
    uint8_t     ep1_interval;   /* Interval bInterval du descripteur     */
    uint16_t    ep1_mps;        /* MaxPacketSize EP1 IN                  */
    uint8_t     dev_addr;       /* Adresse USB assignÃ©e                  */
    uint8_t     interface_num;  /* NumÃ©ro d'interface HID                */
    uint32_t    op_base;        /* xHC Operational Base                  */
    uint32_t    mmio_base;      /* xHC MMIO Base (cap regs)              */
    uint32_t    db_base;        /* Doorbell Array Base                   */
    uint32_t    rt_base;        /* Runtime Register Base                 */

    /* MÃ©moire allouÃ©e via pmm_alloc() â€” pages physiques identitÃ©-mappÃ©es */
    uint32_t    dcbaap_page;    /* Page pour DC Base Address Array Pointer */
    uint32_t    cmd_ring_page;  /* Command Ring TRBs                     */
    uint32_t    evt_ring_page;  /* Event Ring TRBs                       */
    uint32_t    erst_page;      /* Event Ring Segment Table              */
    uint32_t    dev_ctx_page;   /* Device Context                        */
    uint32_t    input_ctx_page; /* Input Context                         */
    uint32_t    ep0_ring_page;  /* Transfer Ring EP0                     */
    uint32_t    ep1_ring_page;  /* Transfer Ring EP1 IN                  */
    uint32_t    data_buf_page;  /* Tampon pour les donnÃ©es USB (descripteurs, rapport) */
    uint32_t    scratchpad_array_page;
    uint32_t    scratchpad_pages[32];
    uint8_t     scratchpad_count;

    /* Etat OHCI minimal (controle + interrupt IN boot keyboard). */
    uint32_t    ohci_mmio_base;
    uint8_t     ohci_addr;
    uint8_t     ohci_low_speed;
    uint8_t     ohci_intr_pending;
    uint8_t     ohci_intr_toggle;
    uint32_t    ohci_hcca_page;
    uint32_t    ohci_ed_page;
    uint32_t    ohci_td_page;
    uint32_t    ohci_setup_page;

    /* Ã‰tat des rings */
    uint32_t    cmd_enq;        /* Indice enqueue Command Ring           */
    uint8_t     cmd_pcs;        /* Producer Cycle State Command Ring     */
    uint32_t    evt_deq;        /* Indice dequeue Event Ring             */
    uint8_t     evt_ccs;        /* Consumer Cycle State Event Ring       */
    uint32_t    ep0_enq;
    uint8_t     ep0_pcs;
    uint32_t    ep1_enq;
    uint8_t     ep1_pcs;
    int         ep1_pending;    /* 1 si un TD est en attente sur EP1     */

    /* Rapport HID prÃ©cÃ©dent (pour dÃ©tecter les changements) */
    uint8_t     last_report[8];
} HidKbdState;

static HidKbdState g_kbd;

static int evt_pop(Trb *out_trb);

static inline uint32_t mmio_r32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}
static inline void mmio_w32(uint32_t addr, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

static inline uint32_t ctx_stride(void) {
    return g_kbd.ctx_size ? (uint32_t)g_kbd.ctx_size : 32u;
}

static inline void evt_drain(uint32_t max_events) {
    Trb evt;
    for (uint32_t i = 0; i < max_events; ++i) {
        if (!evt_pop(&evt)) {
            break;
        }
    }
}

static uint8_t xhci_scratchpad_count(void) {
    uint32_t hcsparams2 = mmio_r32(g_kbd.mmio_base + XHCI_HCSPARAMS2_REG);
    uint32_t lo = (hcsparams2 >> 27) & 0x1Fu;
    uint32_t hi = (hcsparams2 >> 21) & 0x1Fu;
    uint32_t count = lo | (hi << 5);
    if (count > 32u) count = 32u;
    return (uint8_t)count;
}

static int xhci_setup_scratchpads(void) {
    uint8_t count = xhci_scratchpad_count();
    g_kbd.scratchpad_count = count;
    if (!count) return 1;

    g_kbd.scratchpad_array_page = pmm_alloc();
    if (!g_kbd.scratchpad_array_page) return 0;
    kmemset((void *)(uintptr_t)g_kbd.scratchpad_array_page, 0, PAGE_SIZE);

    uint32_t *array = (uint32_t *)(uintptr_t)g_kbd.scratchpad_array_page;
    for (uint8_t i = 0; i < count; ++i) {
        g_kbd.scratchpad_pages[i] = pmm_alloc();
        if (!g_kbd.scratchpad_pages[i]) return 0;
        kmemset((void *)(uintptr_t)g_kbd.scratchpad_pages[i], 0, PAGE_SIZE);
        array[i * 2u] = g_kbd.scratchpad_pages[i];
        array[i * 2u + 1u] = 0;
    }

    ((uint32_t *)(uintptr_t)g_kbd.dcbaap_page)[0] = g_kbd.scratchpad_array_page;
    return 1;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * ACCÃˆS MMIO
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */


static inline uint32_t portsc_addr(uint8_t port) {
    return g_kbd.op_base + XHCI_PORTSC_BASE + (uint32_t)port * XHCI_PORTSC_STRIDE;
}

static uint32_t xhci_portsc_read(uint8_t port) {
    return mmio_r32(portsc_addr(port));
}

static int xhci_port_ready(uint8_t port) {
    uint32_t ps = xhci_portsc_read(port);
    return ((ps & PORTSC_CCS) && (ps & PORTSC_PED) && !(ps & PORTSC_PR)) ? 1 : 0;
}

static void xhci_log_portsc(const char *tag, uint8_t port, uint32_t ps) {
    kprintf("usb_hid: %s port=%u raw=%08x ccs=%u ped=%u pr=%u prc=%u spd=%u\n",
            tag ? tag : "port",
            (unsigned)(port + 1u),
            (unsigned)ps,
            (unsigned)((ps & PORTSC_CCS) ? 1u : 0u),
            (unsigned)((ps & PORTSC_PED) ? 1u : 0u),
            (unsigned)((ps & PORTSC_PR) ? 1u : 0u),
            (unsigned)((ps & PORTSC_PRC) ? 1u : 0u),
            (unsigned)((ps >> 10) & 0x0Fu));
}

static void doorbell(uint8_t slot, uint8_t ep_id) {
    /* Doorbell : slot 0 = Host Controller, slot N = device N
       Valeur : ep_id = 0 pour command ring, 1 pour EP0, 3 pour EP1 IN (DCI) */
    uint32_t addr = g_kbd.db_base + (uint32_t)slot * 4u;
    mmio_w32(addr, (uint32_t)ep_id);
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * DÃ‰LAI ACTIF (sans timer PIT ici)
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static void xhci_delay(uint32_t iterations) {
    for (volatile uint32_t i = 0; i < iterations; ++i) {
        __asm__ volatile ("pause");
    }
}

/* ~1 ms Ã  ~1 GHz (trÃ¨s approximatif, suffisant pour les timeouts USB) */
#define DELAY_1MS   50000u
#define DELAY_10MS  500000u
#define DELAY_100MS 5000000u

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * GESTION DU COMMAND RING
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static Trb *cmd_ring(void) {
    return (Trb *)(uintptr_t)g_kbd.cmd_ring_page;
}

static void cmd_enqueue(uint32_t p0, uint32_t p1, uint32_t status, uint32_t ctrl_no_cycle) {
    Trb *ring = cmd_ring();
    uint32_t idx = g_kbd.cmd_enq;

    /* Si on arrive sur le TRB Link (dernier), on tourne */
    if (idx >= CMD_RING_TRBS - 1u) {
        /* Ã‰crire le Link TRB avec le bit Toggle Cycle */
        ring[CMD_RING_TRBS - 1u].param_lo = g_kbd.cmd_ring_page;
        ring[CMD_RING_TRBS - 1u].param_hi = 0;
        ring[CMD_RING_TRBS - 1u].status   = 0;
        ring[CMD_RING_TRBS - 1u].control  = TRB_TYPE(TRBTYPE_LINK) |
                                            (1u << 1) |  /* Toggle Cycle */
                                            (uint32_t)g_kbd.cmd_pcs;
        g_kbd.cmd_pcs ^= 1u;
        idx = 0;
        g_kbd.cmd_enq = 0;
    }

    ring[idx].param_lo = p0;
    ring[idx].param_hi = p1;
    ring[idx].status   = status;
    ring[idx].control  = ctrl_no_cycle | (uint32_t)g_kbd.cmd_pcs;
    g_kbd.cmd_enq = idx + 1u;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * ATTENTE D'Ã‰VÃ‰NEMENT SUR L'EVENT RING
 * Retourne 1 si l'Ã©vÃ©nement attendu est arrivÃ©, 0 si timeout.
 * out_trb reÃ§oit le TRB Ã©vÃ©nement.
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static uint8_t evt_type(const Trb *trb) {
    return (uint8_t)((trb->control >> TRB_TYPE_SHIFT) & 0x3Fu);
}

static int evt_pop(Trb *out_trb) {
    Trb *ring = (Trb *)(uintptr_t)g_kbd.evt_ring_page;
    Trb *e = &ring[g_kbd.evt_deq];
    uint8_t cycle = (uint8_t)(e->control & 1u);

    if (cycle != g_kbd.evt_ccs) {
        return 0;
    }

    if (out_trb) *out_trb = *e;
    g_kbd.evt_deq++;
    if (g_kbd.evt_deq >= EVT_RING_TRBS) {
        g_kbd.evt_deq = 0;
        g_kbd.evt_ccs ^= 1u;
    }
    /* Mise Ã  jour ERDP (bit 3 = EHB Ã  Ã©crire pour effacer) */
    {
        uint32_t erdp = g_kbd.evt_ring_page + g_kbd.evt_deq * sizeof(Trb);
        mmio_w32(g_kbd.rt_base + 0x20u + IR0_ERDP_LO, erdp | (1u << 3));
        mmio_w32(g_kbd.rt_base + 0x20u + IR0_ERDP_HI, 0);
    }
    return 1;
}

static int evt_wait(uint32_t timeout_iters, Trb *out_trb) {
    for (uint32_t t = 0; t < timeout_iters; ++t) {
        if (evt_pop(out_trb)) {
            return 1;
        }
        __asm__ volatile ("pause");
    }
    return 0; /* timeout */
}

static int evt_wait_type(uint32_t timeout_iters, uint8_t expected_type, Trb *out_trb) {
    Trb evt;

    for (uint32_t t = 0; t < timeout_iters; ++t) {
        if (!evt_pop(&evt)) {
            __asm__ volatile ("pause");
            continue;
        }
        if (evt_type(&evt) == expected_type) {
            if (out_trb) *out_trb = evt;
            return 1;
        }
    }
    return 0;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * COMMANDES xHCI
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

/* Enable Slot â†’ retourne slot_id (1-255) ou 0 si Ã©chec */
static uint8_t xhci_enable_slot(void) {
    cmd_enqueue(0, 0, 0, TRB_TYPE(TRBTYPE_ENABLE_SLOT) | TRB_IOC);
    doorbell(0, 0);

    Trb evt;
    if (!evt_wait_type(DELAY_100MS, EVTYPE_CMD_COMPLETE, &evt)) {
        kprintf("usb_hid: Enable Slot timeout\n");
        return 0;
    }

    uint8_t cc = (uint8_t)((evt.status >> 24) & 0xFFu);
    uint8_t sid = (uint8_t)((evt.control >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        kprintf("usb_hid: Enable Slot cc=%u\n", (unsigned)cc);
        return 0;
    }
    return sid;
}

/* Address Device (BSR = Block Set Address Request)
   BSR=1 : seulement initialiser le slot (Default state)
   BSR=0 : SET_ADDRESS aussi (Addressed state) */
static int xhci_address_device(uint8_t slot_id, uint32_t input_ctx_phys, int bsr) {
    uint32_t ctrl = TRB_TYPE(TRBTYPE_ADDRESS_DEV) | TRB_IOC |
                    ((uint32_t)slot_id << 24) |
                    (bsr ? (1u << 9) : 0u);
    cmd_enqueue(input_ctx_phys, 0, 0, ctrl);
    doorbell(0, 0);

    Trb evt;
    if (!evt_wait_type(DELAY_100MS, EVTYPE_CMD_COMPLETE, &evt)) {
        kprintf("usb_hid: Address Device timeout bsr=%u\n", (unsigned)(bsr ? 1u : 0u));
        return 0;
    }
    uint8_t cc = (uint8_t)((evt.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        kprintf("usb_hid: Address Device cc=%u bsr=%u\n",
                (unsigned)cc,
                (unsigned)(bsr ? 1u : 0u));
    }
    return (cc == CC_SUCCESS) ? 1 : 0;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * TRANSFERS DE CONTRÃ”LE EP0
 * SÃ©quence : SETUP TRB â†’ DATA TRB (optionnel) â†’ STATUS TRB
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static Trb *ep0_ring(void) {
    return (Trb *)(uintptr_t)g_kbd.ep0_ring_page;
}

static void ep0_enqueue(uint32_t p0, uint32_t p1, uint32_t status, uint32_t ctrl_no_cycle) {
    Trb *ring = ep0_ring();
    uint32_t idx = g_kbd.ep0_enq;
    if (idx >= EP0_RING_TRBS - 1u) {
        ring[EP0_RING_TRBS - 1u].param_lo = g_kbd.ep0_ring_page;
        ring[EP0_RING_TRBS - 1u].param_hi = 0;
        ring[EP0_RING_TRBS - 1u].status   = 0;
        ring[EP0_RING_TRBS - 1u].control  = TRB_TYPE(TRBTYPE_LINK) |
                                             (1u << 1) |
                                             (uint32_t)g_kbd.ep0_pcs;
        g_kbd.ep0_pcs ^= 1u;
        idx = 0;
        g_kbd.ep0_enq = 0;
    }
    ring[idx].param_lo = p0;
    ring[idx].param_hi = p1;
    ring[idx].status   = status;
    ring[idx].control  = ctrl_no_cycle | (uint32_t)g_kbd.ep0_pcs;
    g_kbd.ep0_enq = idx + 1u;
}

/*
 * control_transfer() â€” envoie un SETUP stage + DATA stage (IN) + STATUS stage
 *
 *  bmRequestType, bRequest, wValue, wIndex, wLength : champs USB SETUP packet
 *  data_phys : adresse physique du tampon de rÃ©ception (ou 0 si wLength==0)
 *  Retourne 1 si succÃ¨s, 0 si erreur/timeout.
 */
static int control_transfer(
    uint8_t bmRequestType, uint8_t bRequest,
    uint16_t wValue, uint16_t wIndex, uint16_t wLength,
    uint32_t data_phys)
{
    uint32_t setup_lo = (uint32_t)bmRequestType |
                        ((uint32_t)bRequest   << 8)  |
                        ((uint32_t)wValue     << 16);
    uint32_t setup_hi = (uint32_t)wIndex |
                        ((uint32_t)wLength << 16);
    uint32_t setup_status = 8u;

    uint8_t trt = (wLength == 0) ? 0u : ((bmRequestType & 0x80u) ? 3u : 2u);
    uint32_t setup_ctrl = TRB_TYPE(TRBTYPE_SETUP) | TRB_IDT |
                          ((uint32_t)trt << 16);
    ep0_enqueue(setup_lo, setup_hi, setup_status, setup_ctrl);

    if (wLength > 0 && data_phys) {
        uint32_t data_status = (uint32_t)wLength;
        uint32_t data_ctrl = TRB_TYPE(TRBTYPE_DATA) | TRB_ISP;
        if (bmRequestType & 0x80u) data_ctrl |= TRB_DIR_IN;
        ep0_enqueue(data_phys, 0, data_status, data_ctrl);
    }

    {
        uint32_t status_ctrl = TRB_TYPE(TRBTYPE_STATUS) | TRB_IOC;
        if (wLength == 0 || !(bmRequestType & 0x80u)) status_ctrl |= TRB_DIR_IN;
        ep0_enqueue(0, 0, 0, status_ctrl);
    }

    doorbell((uint8_t)g_kbd.slot_id, 1u);

    Trb evt;
    if (!evt_wait_type(DELAY_100MS, EVTYPE_TRANSFER, &evt)) {
        kprintf("usb_hid: EP0 xfer timeout req=%u len=%u\n",
                (unsigned)bRequest,
                (unsigned)wLength);
        return 0;
    }

    {
        uint8_t cc = (uint8_t)((evt.status >> 24) & 0xFFu);
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
            kprintf("usb_hid: EP0 xfer cc=%u req=%u len=%u\n",
                    (unsigned)cc,
                    (unsigned)bRequest,
                    (unsigned)wLength);
            return 0;
        }
    }

    return 1;
}

/* Raccourcis USB standard */
#define USB_REQ_GET_DESCRIPTOR  0x06u
#define USB_REQ_SET_CONFIGURATION 0x09u
#define USB_DESC_DEVICE         0x0100u
#define USB_DESC_CONFIG         0x0200u

/* HID class requests */
#define HID_REQ_SET_PROTOCOL    0x0Bu
#define HID_REQ_SET_IDLE        0x0Au
#define HID_REQ_GET_REPORT      0x01u
#define HID_BOOT_PROTOCOL       0x00u
#define HID_REPORT_PROTOCOL     0x01u

/* bmRequestType */
#define RT_HOST_TO_DEV_STD_DEV  0x00u   /* Hostâ†’Device, Standard, Device   */
#define RT_DEV_TO_HOST_STD_DEV  0x80u   /* Deviceâ†’Host, Standard, Device   */
#define RT_HOST_TO_DEV_CLS_IF   0x21u   /* Hostâ†’Device, Class,    Interface */
#define RT_DEV_TO_HOST_CLS_IF   0xA1u   /* Deviceâ†’Host, Class,    Interface */

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * PARSE DES DESCRIPTEURS USB
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

/* Descripteur Device (18 octets) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} UsbDeviceDesc;

/* Descripteur Configuration (9 octets) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} UsbConfigDesc;

/* Descripteur Interface (9 octets) */
typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} UsbInterfaceDesc;

/* Descripteur Endpoint (7 octets) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} UsbEndpointDesc;

#define DESC_TYPE_DEVICE        0x01u
#define DESC_TYPE_CONFIG        0x02u
#define DESC_TYPE_INTERFACE     0x04u
#define DESC_TYPE_ENDPOINT      0x05u
#define DESC_TYPE_HID           0x21u

/*
 * Parcourt le tampon de configuration pour trouver une interface HID Boot Keyboard.
 * Remplit g_kbd.interface_num, g_kbd.ep1_addr, g_kbd.ep1_mps, g_kbd.ep1_interval.
 * Retourne 1 si trouvÃ©.
 */
static int parse_config_descriptor(const uint8_t *buf, uint16_t len) {
    uint16_t off = 0;
    int in_hid_boot_kbd = 0;
    uint8_t iface_num = 0;

    while (off + 2u <= len) {
        uint8_t desc_len  = buf[off];
        uint8_t desc_type = buf[off + 1u];
        if (desc_len == 0 || off + desc_len > len) break;

        if (desc_type == DESC_TYPE_INTERFACE && desc_len >= 9u) {
            const UsbInterfaceDesc *id = (const UsbInterfaceDesc *)(const void *)(buf + off);
            /* HID (3), Boot Interface Subclass (1), Keyboard (1) */
            in_hid_boot_kbd = (id->bInterfaceClass    == 3u &&
                               id->bInterfaceSubClass == 1u &&
                               id->bInterfaceProtocol == 1u) ? 1 : 0;
            if (in_hid_boot_kbd) iface_num = id->bInterfaceNumber;
        }

        if (in_hid_boot_kbd && desc_type == DESC_TYPE_ENDPOINT && desc_len >= 7u) {
            const UsbEndpointDesc *ed = (const UsbEndpointDesc *)(const void *)(buf + off);
            /* Interrupt IN : bits[1:0]=3 (interrupt), bit7=1 (IN) */
            if ((ed->bmAttributes & 0x03u) == 0x03u &&
                (ed->bEndpointAddress & 0x80u) != 0) {
                g_kbd.interface_num = iface_num;
                g_kbd.ep1_addr      = ed->bEndpointAddress;
                g_kbd.ep1_mps       = ed->wMaxPacketSize & 0x07FFu;
                g_kbd.ep1_interval  = ed->bInterval;
                return 1;
            }
        }

        off += desc_len;
    }
    return 0;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * INITIALISATION DU DEVICE CONTEXT + INPUT CONTEXT
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

/*
 * Calcule le Max Packet Size EP0 en fonction de la vitesse.
 * Full/Low Speed : 8, High Speed : 64, SuperSpeed : 512.
 */
static uint16_t ep0_max_packet_size(uint8_t speed) {
    switch (speed) {
        case SPEED_LOW:   return 8u;
        case SPEED_FULL:  return 8u;
        case SPEED_HIGH:  return 64u;
        case SPEED_SUPER: return 512u;
        default:          return 8u;
    }
}

/*
 * PrÃ©pare l'Input Context pour Address Device.
 * Seul EP0 est activÃ© dans cette passe (A0 + A1 flags).
 */
static void setup_input_context_ep0(uint16_t mps_override) {
    uint8_t *ic = (uint8_t *)(uintptr_t)g_kbd.input_ctx_page;
    uint32_t csz = ctx_stride();
    kmemset(ic, 0, PAGE_SIZE);

    /* Input Control Context : Add Context Flags A0 (Slot) + A1 (EP0) */
    uint32_t *icc = (uint32_t *)(void *)ic;
    icc[1] = (1u << 0) | (1u << 1);   /* A0=Slot, A1=EP0 */

    /* Slot Context (offset = 1 contexte, juste apres l'Input Control Context) */
    SlotCtx *sc = (SlotCtx *)(void *)(ic + csz);
    /* Speed dans bits[23:20], Context Entries = 1 dans bits[31:27]
       RH Port Number dans bits[23:16] de dw1 */
    sc->dw0 = ((uint32_t)g_kbd.speed    << 20) |
              (1u                        << 27); /* ContextEntries=1 (EP0 seulement) */
    sc->dw1 = ((uint32_t)(g_kbd.port_idx + 1u) << 16); /* Port Number (1-based) */

    /* EP0 Context (DCI=1, offset = 2 contextes) */
    EpCtx *ep0 = (EpCtx *)(void *)(ic + (2u * csz));
    uint16_t mps = mps_override ? mps_override : ep0_max_packet_size(g_kbd.speed);
    ep0->dw0 = 0;   /* Interval=0, MaxPStreams=0 */
    ep0->dw1 = (EP_TYPE_CTRL << 3) |
               ((uint32_t)mps << 16) |
               (3u << 1);           /* CErr=3 */
    ep0->tr_deq_lo = g_kbd.ep0_ring_page | 1u;  /* DCS=1 */
    ep0->tr_deq_hi = 0;
    ep0->dw4 = 8u;  /* Average TRB Length = 8 (SETUP size) */
}

static uint16_t ep0_mps_from_device_desc(uint8_t speed, uint8_t desc_mps) {
    switch (speed) {
        case SPEED_LOW:
            return 8u;
        case SPEED_FULL:
            if (desc_mps == 8u || desc_mps == 16u || desc_mps == 32u || desc_mps == 64u) {
                return (uint16_t)desc_mps;
            }
            return 8u;
        case SPEED_HIGH:
            return 64u;
        case SPEED_SUPER:
            if (desc_mps >= 9u && desc_mps <= 16u) {
                return (uint16_t)(1u << desc_mps);
            }
            return 512u;
        default:
            return ep0_max_packet_size(speed);
    }
}

static void ep0_reset_ring(void) {
    kmemset((void *)(uintptr_t)g_kbd.ep0_ring_page, 0, PAGE_SIZE);
    g_kbd.ep0_enq = 0;
    g_kbd.ep0_pcs = 1u;
}

static void ep1_reset_ring(void) {
    kmemset((void *)(uintptr_t)g_kbd.ep1_ring_page, 0, PAGE_SIZE);
    g_kbd.ep1_enq = 0;
    g_kbd.ep1_pcs = 1u;
    g_kbd.ep1_pending = 0;
}

static void reset_attempt_state(void) {
    kmemset((void *)(uintptr_t)g_kbd.dev_ctx_page, 0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.input_ctx_page, 0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.data_buf_page, 0, PAGE_SIZE);
    ep0_reset_ring();
    ep1_reset_ring();
    g_kbd.slot_id = 0;
    g_kbd.dev_addr = 0;
    g_kbd.interface_num = 0;
    g_kbd.ep1_addr = 0;
    g_kbd.ep1_interval = 0;
    g_kbd.ep1_mps = 0;
    evt_drain(EVT_RING_TRBS * 2u);
}

static void dcbaap_set_slot_ctx(uint8_t slot_id, uint32_t ctx_phys) {
    uint32_t *dcbaap = (uint32_t *)(uintptr_t)g_kbd.dcbaap_page;
    uint32_t idx = (uint32_t)slot_id * 2u;
    dcbaap[idx] = ctx_phys;
    dcbaap[idx + 1u] = 0;
}

/*
 * PrÃ©pare l'Input Context pour Configure Endpoint (ajouter EP1 IN).
 * AppelÃ© aprÃ¨s Address Device rÃ©ussi.
 */
static void setup_input_context_ep1(void) {
    uint8_t *ic = (uint8_t *)(uintptr_t)g_kbd.input_ctx_page;
    uint32_t csz = ctx_stride();
    /* Garder le Slot Context, ajouter EP1 IN */

    /* Input Control Context : Add A0 (Slot) + A1 (EP0) + A3 (EP1 IN, DCI=3) */
    uint32_t *icc = (uint32_t *)(void *)ic;
    icc[1] = (1u << 0) | (1u << 1) | (1u << 3);

    /* Mettre Ã  jour ContextEntries dans Slot Context (maintenant = 3) */
    SlotCtx *sc = (SlotCtx *)(void *)(ic + csz);
    sc->dw0 = (sc->dw0 & ~(0x1Fu << 27)) | (3u << 27);

    /* EP1 IN Context (DCI=3, offset = 1 contexte de controle + 3 contextes) */
    EpCtx *ep1 = (EpCtx *)(void *)(ic + (4u * csz));
    kmemset(ep1, 0, sizeof(EpCtx));
    uint16_t mps = (g_kbd.ep1_mps > 0) ? g_kbd.ep1_mps : 8u;
    ep1->dw0 = ((uint32_t)g_kbd.ep1_interval << 16);
    ep1->dw1 = (EP_TYPE_INT_IN << 3) |
               ((uint32_t)mps << 16) |
               (3u << 1);   /* CErr=3 */
    ep1->tr_deq_lo = g_kbd.ep1_ring_page | 1u;  /* DCS=1 */
    ep1->tr_deq_hi = 0;
    ep1->dw4 = (uint32_t)mps;  /* Average TRB Length = MaxPacketSize */
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * CONFIGURE ENDPOINT COMMAND
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static int xhci_configure_ep(uint8_t slot_id, uint32_t input_ctx_phys) {
    uint32_t ctrl = TRB_TYPE(TRBTYPE_CONFIG_EP) | TRB_IOC |
                    ((uint32_t)slot_id << 24);
    cmd_enqueue(input_ctx_phys, 0, 0, ctrl);
    doorbell(0, 0);

    Trb evt;
    if (!evt_wait_type(DELAY_100MS, EVTYPE_CMD_COMPLETE, &evt)) {
        kprintf("usb_hid: Configure Endpoint timeout\n");
        return 0;
    }
    uint8_t cc = (uint8_t)((evt.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        kprintf("usb_hid: Configure Endpoint cc=%u\n", (unsigned)cc);
    }
    return (cc == CC_SUCCESS) ? 1 : 0;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * RESET DE PORT
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static int port_reset(uint8_t port_idx) {
    uint32_t paddr = portsc_addr(port_idx);
    uint32_t ps = mmio_r32(paddr);

    if (!(ps & PORTSC_CCS)) return 0;

    if (!(ps & PORTSC_PP)) {
        mmio_w32(paddr, PORTSC_PP);
        xhci_delay(DELAY_100MS);
        ps = mmio_r32(paddr);
    }

    /* Debounce du port apres RUN/handoff: utile sur vrai materiel. */
    xhci_delay(DELAY_100MS);
    ps = mmio_r32(paddr);

    if ((ps & PORTSC_PED) && !(ps & PORTSC_PR)) {
        if (ps & PORTSC_W1C_MASK) {
            mmio_w32(paddr, ps & PORTSC_W1C_MASK);
        }
        return 1;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ps & PORTSC_W1C_MASK) {
            mmio_w32(paddr, ps & PORTSC_W1C_MASK);
            ps = mmio_r32(paddr);
        }

        {
            uint32_t wr = PORTSC_PR;
            if (ps & PORTSC_PP) wr |= PORTSC_PP;
            mmio_w32(paddr, wr);
        }

        for (int t = 0; t < 2000; ++t) {
            xhci_delay(DELAY_1MS);
            ps = mmio_r32(paddr);
            if ((ps & PORTSC_PED) && !(ps & PORTSC_PR)) {
                if (ps & PORTSC_W1C_MASK) {
                    mmio_w32(paddr, ps & PORTSC_W1C_MASK);
                }
                return 1;
            }
            if (ps & PORTSC_PRC) {
                uint32_t clear = PORTSC_PRC;
                if (ps & PORTSC_PP) clear |= PORTSC_PP;
                mmio_w32(paddr, clear);
                if (ps & PORTSC_PED) {
                    return 1;
                }
                break;
            }
        }

        /* Petit temps de repos avant une seconde tentative. */
        xhci_delay(DELAY_100MS);
        ps = mmio_r32(paddr);
        if (!(ps & PORTSC_CCS)) {
            break;
        }
    }

    return 0;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * TRANSFER EP1 IN (INTERRUPT)
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static Trb *ep1_ring(void) {
    return (Trb *)(uintptr_t)g_kbd.ep1_ring_page;
}

/*
 * Soumet un Normal TRB sur EP1 IN pour recevoir le prochain rapport HID.
 * data_phys : tampon de 8 octets minimum.
 */
static void ep1_submit(uint32_t data_phys) {
    if (g_kbd.ep1_pending) return;

    Trb *ring = ep1_ring();
    uint32_t idx = g_kbd.ep1_enq;

    if (idx >= EP1_RING_TRBS - 1u) {
        ring[EP1_RING_TRBS - 1u].param_lo = g_kbd.ep1_ring_page;
        ring[EP1_RING_TRBS - 1u].param_hi = 0;
        ring[EP1_RING_TRBS - 1u].status   = 0;
        ring[EP1_RING_TRBS - 1u].control  = TRB_TYPE(TRBTYPE_LINK) |
                                             (1u << 1) |
                                             (uint32_t)g_kbd.ep1_pcs;
        g_kbd.ep1_pcs ^= 1u;
        idx = 0;
        g_kbd.ep1_enq = 0;
    }

    uint16_t mps = (g_kbd.ep1_mps > 0 && g_kbd.ep1_mps <= 8u) ? g_kbd.ep1_mps : 8u;
    ring[idx].param_lo = data_phys;
    ring[idx].param_hi = 0;
    ring[idx].status   = (uint32_t)mps;
    ring[idx].control  = TRB_TYPE(TRBTYPE_NORMAL) | TRB_ISP | TRB_IOC |
                         (uint32_t)g_kbd.ep1_pcs;
    g_kbd.ep1_enq = idx + 1u;
    g_kbd.ep1_pending = 1;

    /* DCI pour EP1 IN = (ep_num * 2) + direction = (1*2)+1 = 3 */
    doorbell((uint8_t)g_kbd.slot_id, 3u);
}

/*
 * VÃ©rifie si un Ã©vÃ©nement Transfer est disponible pour EP1 IN.
 * Retourne 1 si un rapport a Ã©tÃ© reÃ§u, 0 sinon.
 */
static int ep1_check_event(void) {
    if (!g_kbd.ep1_pending) return 0;

    Trb e;
    if (!evt_wait_type(DELAY_1MS, EVTYPE_TRANSFER, &e)) {
        return 0;
    }

    g_kbd.ep1_pending = 0;

    uint8_t cc = (uint8_t)((e.status >> 24) & 0xFFu);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 1 : 0;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * TABLE HID USAGE ID (Keyboard/Keypad Page 0x07) â†’ KEYCODES ALOS
 *
 * Layout physique AZERTY :
 *   HID 0x04 = touche physique 'A' â†’ 'q' en AZERTY
 *   HID 0x1D = touche physique 'Z' â†’ 'w' en AZERTY
 *   etc.
 *
 * On mappe ici vers le keycode ALOS qu'on veut injecter.
 * keyboard.c appliquera lui-mÃªme les modificateurs SHIFT/CAPS une fois
 * qu'on aura injectÃ© les bons keycodes de base.
 *
 * Pour les touches qui gÃ©nÃ¨rent des caractÃ¨res dÃ©pendant du layout,
 * on injecte directement le caractÃ¨re AZERTY non-shiftÃ©.
 * Les modificateurs SHIFT/CTRL/ALT sont gÃ©rÃ©s sÃ©parÃ©ment via
 * keyboard_set_external_modifiers().
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static const uint8_t g_hid_to_alos[256] = {
    /* 0x00 */ KEY_NONE,        /* Reserved (no event) */
    /* 0x01 */ KEY_NONE,        /* Keyboard Error Roll Over */
    /* 0x02 */ KEY_NONE,        /* Keyboard POST Fail */
    /* 0x03 */ KEY_NONE,        /* Keyboard Error Undefined */
    /* 0x04 */ 'q',             /* A â†’ Q (AZERTY) */
    /* 0x05 */ 'b',             /* B */
    /* 0x06 */ 'c',             /* C */
    /* 0x07 */ 'd',             /* D */
    /* 0x08 */ 'e',             /* E */
    /* 0x09 */ 'f',             /* F */
    /* 0x0A */ 'g',             /* G */
    /* 0x0B */ 'h',             /* H */
    /* 0x0C */ 'i',             /* I */
    /* 0x0D */ 'j',             /* J */
    /* 0x0E */ 'k',             /* K */
    /* 0x0F */ 'l',             /* L */
    /* 0x10 */ ',',             /* M â†’ , (AZERTY: M est Ã  la position de ,) */
    /* 0x11 */ 'n',             /* N */
    /* 0x12 */ 'o',             /* O */
    /* 0x13 */ 'p',             /* P */
    /* 0x14 */ 'a',             /* Q â†’ A (AZERTY) */
    /* 0x15 */ 'r',             /* R */
    /* 0x16 */ 's',             /* S */
    /* 0x17 */ 't',             /* T */
    /* 0x18 */ 'u',             /* U */
    /* 0x19 */ 'v',             /* V */
    /* 0x1A */ 'z',             /* W â†’ Z (AZERTY) */
    /* 0x1B */ 'x',             /* X */
    /* 0x1C */ 'y',             /* Y */
    /* 0x1D */ 'w',             /* Z â†’ W (AZERTY) */
    /* 0x1E */ '&',             /* 1 â†’ & (AZERTY) */
    /* 0x1F */ 0xE9,            /* 2 â†’ Ã© (AZERTY) */
    /* 0x20 */ '"',             /* 3 â†’ " (AZERTY) */
    /* 0x21 */ '\'',            /* 4 â†’ ' (AZERTY) */
    /* 0x22 */ '(',             /* 5 â†’ ( (AZERTY) */
    /* 0x23 */ '-',             /* 6 â†’ - (AZERTY) */
    /* 0x24 */ 0xE8,            /* 7 â†’ Ã¨ (AZERTY) */
    /* 0x25 */ '_',             /* 8 â†’ _ (AZERTY) */
    /* 0x26 */ 0xE7,            /* 9 â†’ Ã§ (AZERTY) */
    /* 0x27 */ 0xE0,            /* 0 â†’ Ã  (AZERTY) */
    /* 0x28 */ KEY_ENTER,       /* Return */
    /* 0x29 */ KEY_ESCAPE,      /* Escape */
    /* 0x2A */ KEY_BACKSPACE,   /* Backspace */
    /* 0x2B */ KEY_TAB,         /* Tab */
    /* 0x2C */ ' ',             /* Space */
    /* 0x2D */ ')',             /* - â†’ ) (AZERTY: position = Minus key) */
    /* 0x2E */ '=',             /* = (AZERTY) */
    /* 0x2F */ '^',             /* [ â†’ ^ (AZERTY: position] */
    /* 0x30 */ '$',             /* ] â†’ $ (AZERTY) */
    /* 0x31 */ '*',             /* \ â†’ * (AZERTY) */
    /* 0x32 */ KEY_NONE,        /* Non-US # (pas mappÃ© AZERTY simple) */
    /* 0x33 */ 'm',             /* ; â†’ M (AZERTY: ; est Ã  la position de M) */
    /* 0x34 */ 0xF9,            /* ' â†’ Ã¹ (AZERTY) */
    /* 0x35 */ '*',             /* ` â†’ * (AZERTY: touche Â² / * ) */
    /* 0x36 */ ';',             /* , â†’ ; (AZERTY) */
    /* 0x37 */ ':',             /* . â†’ : (AZERTY) */
    /* 0x38 */ '!',             /* / â†’ ! (AZERTY) */
    /* 0x39 */ KEY_NONE,        /* Caps Lock (gÃ©rÃ© comme modificateur) */
    /* 0x3A */ KEY_F1,
    /* 0x3B */ KEY_F2,
    /* 0x3C */ KEY_F3,
    /* 0x3D */ KEY_F4,
    /* 0x3E */ KEY_F5,
    /* 0x3F */ KEY_F6,
    /* 0x40 */ KEY_F7,
    /* 0x41 */ KEY_F8,
    /* 0x42 */ KEY_F9,
    /* 0x43 */ KEY_F10,
    /* 0x44 */ KEY_F11,
    /* 0x45 */ KEY_F12,
    /* 0x46 */ KEY_NONE,        /* Print Screen */
    /* 0x47 */ KEY_NONE,        /* Scroll Lock */
    /* 0x48 */ KEY_NONE,        /* Pause */
    /* 0x49 */ KEY_INSERT,
    /* 0x4A */ KEY_HOME,
    /* 0x4B */ KEY_PGUP,
    /* 0x4C */ KEY_DELETE,
    /* 0x4D */ KEY_END,
    /* 0x4E */ KEY_PGDN,
    /* 0x4F */ KEY_RIGHT,
    /* 0x50 */ KEY_LEFT,
    /* 0x51 */ KEY_DOWN,
    /* 0x52 */ KEY_UP,
    /* 0x53 */ KEY_NONE,        /* Num Lock */
    /* 0x54 */ '/',             /* Keypad / */
    /* 0x55 */ '*',             /* Keypad * */
    /* 0x56 */ '-',             /* Keypad - */
    /* 0x57 */ '+',             /* Keypad + */
    /* 0x58 */ KEY_ENTER,       /* Keypad Enter */
    /* 0x59 */ '1',             /* Keypad 1 */
    /* 0x5A */ '2',             /* Keypad 2 */
    /* 0x5B */ '3',             /* Keypad 3 */
    /* 0x5C */ '4',             /* Keypad 4 */
    /* 0x5D */ '5',             /* Keypad 5 */
    /* 0x5E */ '6',             /* Keypad 6 */
    /* 0x5F */ '7',             /* Keypad 7 */
    /* 0x60 */ '8',             /* Keypad 8 */
    /* 0x61 */ '9',             /* Keypad 9 */
    /* 0x62 */ '0',             /* Keypad 0 */
    /* 0x63 */ '.',             /* Keypad . */
    /* 0x64 */ KEY_NONE,        /* Non-US \ */
    /* 0x65 */ KEY_NONE,        /* Application */
    /* 0x66 */ KEY_NONE,        /* Power */
    /* 0x67 */ '=',             /* Keypad = */
    /* 0x68..0xFF : remplissage KEY_NONE */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0x68-0x6F */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0x70-0x77 */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0x78-0x7F */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0x80-0x87 */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0x88-0x8F */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0x90-0x97 */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0x98-0x9F */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xA0-0xA7 */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xA8-0xAF */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xB0-0xB7 */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xB8-0xBF */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xC0-0xC7 */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xC8-0xCF */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xD0-0xD7 */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xD8-0xDF */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xE0-0xE7 (modifiers, gÃ©rÃ©s sÃ©parÃ©ment) */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xE8-0xEF */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xF0-0xF7 */
    KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE,KEY_NONE, /* 0xF8-0xFF */
};

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * DÃ‰CODAGE DU RAPPORT HID BOOT KEYBOARD
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

/*
 * Modifieur HID (byte 0) â†’ MOD_* ALOS
 *
 *  Bit 0 : L-CTRL  â†’ MOD_CTRL
 *  Bit 1 : L-SHIFT â†’ MOD_SHIFT
 *  Bit 2 : L-ALT   â†’ MOD_ALT
 *  Bit 3 : L-GUI   â†’ (ignorÃ©)
 *  Bit 4 : R-CTRL  â†’ MOD_CTRL
 *  Bit 5 : R-SHIFT â†’ MOD_SHIFT
 *  Bit 6 : R-ALT   â†’ MOD_ALT (AltGr)
 *  Bit 7 : R-GUI   â†’ (ignorÃ©)
 */
static uint8_t hid_mods_to_alos(uint8_t hid_mod) {
    uint8_t m = 0;
    if (hid_mod & 0x01u) m |= MOD_CTRL;
    if (hid_mod & 0x02u) m |= MOD_SHIFT;
    if (hid_mod & 0x04u) m |= MOD_ALT;
    if (hid_mod & 0x10u) m |= MOD_CTRL;
    if (hid_mod & 0x20u) m |= MOD_SHIFT;
    if (hid_mod & 0x40u) m |= MOD_ALT;
    return m;
}

/*
 * Traite un rapport HID de 8 octets.
 * Compare avec last_report pour ne dÃ©clencher que les nouveautÃ©s.
 */
static void process_hid_report(const uint8_t *report) {
    uint8_t new_mods = hid_mods_to_alos(report[0]);
    keyboard_set_external_modifiers(new_mods);

    /* DÃ©terminer les touches relÃ¢chÃ©es : dans last mais plus dans current */
    for (int i = 2; i < 8; ++i) {
        uint8_t old_key = g_kbd.last_report[i];
        if (old_key == 0x00u || old_key == 0x01u) continue; /* vide ou rollover */
        int still_pressed = 0;
        for (int j = 2; j < 8; ++j) {
            if (report[j] == old_key) { still_pressed = 1; break; }
        }
        if (!still_pressed) {
            uint8_t alos_key = g_hid_to_alos[old_key];
            if (alos_key != KEY_NONE) {
                keyboard_inject_release(alos_key);
            }
        }
    }

    /* DÃ©terminer les nouvelles touches pressÃ©es : dans current mais pas dans last */
    for (int i = 2; i < 8; ++i) {
        uint8_t new_key = report[i];
        if (new_key == 0x00u || new_key == 0x01u) continue;
        int was_pressed = 0;
        for (int j = 2; j < 8; ++j) {
            if (g_kbd.last_report[j] == new_key) { was_pressed = 1; break; }
        }
        if (!was_pressed) {
            uint8_t alos_key = g_hid_to_alos[new_key];
            if (alos_key != KEY_NONE) {
                keyboard_inject_press(alos_key);
            }
        }
    }

    kmemcpy(g_kbd.last_report, report, 8u);
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * INITIALISATION DU xHC (Command Ring + Event Ring + DCBAAP)
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

static int xhci_init_rings(void) {
    /* Allouer les pages nÃ©cessaires */
    g_kbd.dcbaap_page    = pmm_alloc();
    g_kbd.cmd_ring_page  = pmm_alloc();
    g_kbd.evt_ring_page  = pmm_alloc();
    g_kbd.erst_page      = pmm_alloc();
    g_kbd.dev_ctx_page   = pmm_alloc();
    g_kbd.input_ctx_page = pmm_alloc();
    g_kbd.ep0_ring_page  = pmm_alloc();
    g_kbd.ep1_ring_page  = pmm_alloc();
    g_kbd.data_buf_page  = pmm_alloc();

    if (!g_kbd.dcbaap_page || !g_kbd.cmd_ring_page || !g_kbd.evt_ring_page ||
        !g_kbd.erst_page   || !g_kbd.dev_ctx_page  || !g_kbd.input_ctx_page ||
        !g_kbd.ep0_ring_page|| !g_kbd.ep1_ring_page|| !g_kbd.data_buf_page) {
        return 0; /* OOM */
    }

    /* ZÃ©roÃ¯ser toutes les pages */
    kmemset((void *)(uintptr_t)g_kbd.dcbaap_page,    0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.cmd_ring_page,  0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.evt_ring_page,  0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.erst_page,      0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.dev_ctx_page,   0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.input_ctx_page, 0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.ep0_ring_page,  0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.ep1_ring_page,  0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.data_buf_page,  0, PAGE_SIZE);

    if (!xhci_setup_scratchpads()) {
        return 0;
    }

    /* DCBAAP : pointeur vers la page Device Context (slot 0 = scratchpad, slot N = device) */
    uint32_t *dcbaap = (uint32_t *)(uintptr_t)g_kbd.dcbaap_page;
    /* On n'utilise qu'un seul slot â€” sera rempli aprÃ¨s Enable Slot */
    (void)dcbaap;

    /* Command Ring : initialiser le cycle bit du premier TRB */
    g_kbd.cmd_enq = 0;
    g_kbd.cmd_pcs = 1u;

    /* Event Ring Segment Table (ERST) : une seule entrÃ©e */
    uint32_t *erst = (uint32_t *)(uintptr_t)g_kbd.erst_page;
    erst[0] = g_kbd.evt_ring_page;     /* Segment Base Address Lo */
    erst[1] = 0;                        /* Segment Base Address Hi */
    erst[2] = EVT_RING_TRBS;           /* Segment Size */
    erst[3] = 0;

    g_kbd.evt_deq = 0;
    g_kbd.evt_ccs = 1u;

    /* EP0/EP1 rings */
    g_kbd.ep0_enq = 0;  g_kbd.ep0_pcs = 1u;
    g_kbd.ep1_enq = 0;  g_kbd.ep1_pcs = 1u;
    g_kbd.ep1_pending = 0;

    /* Lire les offsets DB et Runtime depuis les registres caps */
    uint32_t dboff  = mmio_r32(g_kbd.mmio_base + XHCI_DBOFF_REG) & ~0x3u;
    uint32_t rtsoff = mmio_r32(g_kbd.mmio_base + XHCI_RTSOFF_REG) & ~0x1Fu;
    g_kbd.db_base = g_kbd.mmio_base + dboff;
    g_kbd.rt_base = g_kbd.mmio_base + rtsoff;

    /* VÃ©rifier que le xHC n'est pas en train de se rÃ©initialiser */
    for (int t = 0; t < 100; ++t) {
        if (!(mmio_r32(g_kbd.op_base + XHCI_USBSTS) & XHCI_STS_CNR)) break;
        xhci_delay(DELAY_1MS);
    }

    /* ArrÃªter le xHC si en cours (pour le reconfigurer) */
    {
        uint32_t cmd = mmio_r32(g_kbd.op_base + XHCI_USBCMD);
        if (cmd & XHCI_CMD_RUN) {
            mmio_w32(g_kbd.op_base + XHCI_USBCMD, cmd & ~XHCI_CMD_RUN);
            for (int t = 0; t < 100; ++t) {
                xhci_delay(DELAY_1MS);
                if (mmio_r32(g_kbd.op_base + XHCI_USBSTS) & XHCI_STS_HCH) break;
            }
        }
    }

    /* Reset xHC */
    mmio_w32(g_kbd.op_base + XHCI_USBCMD,
             mmio_r32(g_kbd.op_base + XHCI_USBCMD) | XHCI_CMD_HCRST);
    for (int t = 0; t < 100; ++t) {
        xhci_delay(DELAY_10MS);
        if (!(mmio_r32(g_kbd.op_base + XHCI_USBCMD) & XHCI_CMD_HCRST) &&
            !(mmio_r32(g_kbd.op_base + XHCI_USBSTS) & XHCI_STS_CNR)) break;
    }

    /* CONFIG : autoriser plusieurs slots pour les essais multi-ports. */
    mmio_w32(g_kbd.op_base + XHCI_CONFIG,
             g_kbd.max_slots ? (uint32_t)g_kbd.max_slots : 1u);

    /* DCBAAP */
    mmio_w32(g_kbd.op_base + XHCI_DCBAAP_LO, g_kbd.dcbaap_page);
    mmio_w32(g_kbd.op_base + XHCI_DCBAAP_HI, 0);

    /* Command Ring Control Register : adresse physique + RCS=1 */
    mmio_w32(g_kbd.op_base + XHCI_CRCR_LO, g_kbd.cmd_ring_page | 1u);
    mmio_w32(g_kbd.op_base + XHCI_CRCR_HI, 0);

    /* Interrupter 0 : Event Ring */
    uint32_t ir0 = g_kbd.rt_base + 0x20u;
    mmio_w32(ir0 + IR0_ERSTSZ,    1u);                           /* ERST size = 1 */
    mmio_w32(ir0 + IR0_IMOD,      0u);
    mmio_w32(ir0 + IR0_ERDP_LO,   g_kbd.evt_ring_page);
    mmio_w32(ir0 + IR0_ERDP_HI,   0);
    mmio_w32(ir0 + IR0_ERSTBA_LO, g_kbd.erst_page);
    mmio_w32(ir0 + IR0_ERSTBA_HI, 0);
    mmio_w32(ir0 + IR0_IMAN,      0x00000003u);  /* IE=1, IP=1 (effacer pending) */

    /* DÃ©marrer le xHC : RUN=1 */
    mmio_w32(g_kbd.op_base + XHCI_USBCMD,
             mmio_r32(g_kbd.op_base + XHCI_USBCMD) | XHCI_CMD_RUN | XHCI_CMD_INTE);
    for (int t = 0; t < 100; ++t) {
        xhci_delay(DELAY_1MS);
        if (!(mmio_r32(g_kbd.op_base + XHCI_USBSTS) & XHCI_STS_HCH)) break;
    }

    /* DNCTRL : activer les notifications de Device (bit 1 = Function Wake) */
    mmio_w32(g_kbd.op_base + XHCI_DNCTRL, 0x0002u);

    return 1;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * DÃ‰TECTION DU PORT AVEC UN CLAVIER CONNECTÃ‰
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

/*
 * DÃ©tecte si un port a bien un pÃ©riphÃ©rique connectÃ©.
 * On ne filtre pas sur PP ici: certains contrÃ´leurs exposent le pÃ©riphÃ©rique
 * avant que l'alimentation de port soit explicitement relancÃ©e cÃ´tÃ© OS.
 */
static int port_connected(uint8_t port_idx, uint8_t *speed_out) {
    uint32_t ps = mmio_r32(portsc_addr(port_idx));
    if (!(ps & PORTSC_CCS)) return 0;

    if (speed_out) {
        uint8_t spd = (uint8_t)((ps >> 10) & 0x0Fu);
        *speed_out = spd ? spd : SPEED_FULL;
    }
    return 1;
}

/*
 * Cherche le premier port xHCI connectÃ©.
 * Retourne l'index du port (0-based) ou -1 si aucun.
 * Remplit g_kbd.speed.
 */
static int find_connected_port(uint8_t max_ports) {
    for (uint8_t p = 0; p < max_ports && p < 16u; ++p) {
        uint8_t spd = 0;
        if (port_connected(p, &spd)) {
            g_kbd.speed = spd;
            return (int)p;
        }
    }
    return -1;
}

static int port_try_priority(uint8_t speed) {
    switch (speed) {
        case SPEED_LOW:
        case SPEED_FULL:
        case SPEED_HIGH:
            return 0;
        case SPEED_SUPER:
        default:
            return 1;
    }
}

static void log_ehci_connected_ports(void) {
    int count = usb_probe_count();
    int any = 0;

    for (int i = 0; i < count; ++i) {
        const UsbHostControllerInfo *cur = usb_probe_get(i);
        if (!cur || cur->kind != USB_HC_EHCI ||
            !cur->legacy_handoff_ok ||
            !cur->mmio_base || cur->mmio_base > 0xFFFFFFFFull) {
            continue;
        }

        uint32_t mmio = (uint32_t)cur->mmio_base;
        uint32_t op_base = mmio + (uint32_t)cur->cap_length;
        int connected = 0;

        for (uint8_t port = 0; port < cur->port_count && port < 16u; ++port) {
            uint32_t ps = mmio_r32(op_base + EHCI_PORTSC_BASE + ((uint32_t)port * EHCI_PORTSC_STRIDE));
            if (ps & EHCI_PORTSC_CCS) {
                connected++;
                kprintf("usb_hid: EHCI b%u s%u port=%u ccs=1 pe=%u owner=%u raw=%08x\n",
                        (unsigned)cur->bus,
                        (unsigned)cur->slot,
                        (unsigned)(port + 1u),
                        (unsigned)((ps & EHCI_PORTSC_PE) ? 1u : 0u),
                        (unsigned)((ps & EHCI_PORTSC_OWNER) ? 1u : 0u),
                        (unsigned)ps);
            }
        }

        if (connected) {
            any = 1;
        }
    }

    if (!any) {
        kprintf("usb_hid: EHCI aucun port connecte vu\n");
    }
}

static void log_usb_hc_summary(void) {
    int count = usb_probe_count();
    for (int i = 0; i < count; ++i) {
        const UsbHostControllerInfo *cur = usb_probe_get(i);
        if (!cur) continue;
        kprintf("usb_hid: HC %s b%u s%u f%u vid=%x dev=%x pi=%x mmio=%x io=%x ports=%u\n",
                usb_probe_kind_name(cur->kind),
                (unsigned)cur->bus,
                (unsigned)cur->slot,
                (unsigned)cur->func,
                (unsigned)cur->vendor,
                (unsigned)cur->device,
                (unsigned)cur->prog_if,
                (unsigned)(uint32_t)cur->mmio_base,
                (unsigned)cur->io_base,
                (unsigned)cur->port_count);
    }
}

/* ------------------------------------------------------------------------- */
/* OHCI minimal: clavier USB 1.x/2.0 full-speed route vers le compagnon OHCI. */

static inline uint32_t ohci_r32(uint32_t off) {
    return mmio_r32(g_kbd.ohci_mmio_base + off);
}

static inline void ohci_w32(uint32_t off, uint32_t value) {
    mmio_w32(g_kbd.ohci_mmio_base + off, value);
}

static inline uint32_t ohci_port_status_addr(uint8_t port_idx) {
    return g_kbd.ohci_mmio_base + OHCI_RH_PORT_BASE + ((uint32_t)port_idx * 4u);
}

static OhciEd *ohci_ctrl_ed(void) {
    return (OhciEd *)(uintptr_t)g_kbd.ohci_ed_page;
}

static OhciEd *ohci_intr_ed(void) {
    return (OhciEd *)(uintptr_t)(g_kbd.ohci_ed_page + sizeof(OhciEd));
}

static OhciTd *ohci_td(uint8_t idx) {
    return (OhciTd *)(uintptr_t)(g_kbd.ohci_td_page + ((uint32_t)idx * sizeof(OhciTd)));
}

static uint32_t ohci_td_phys(uint8_t idx) {
    return g_kbd.ohci_td_page + ((uint32_t)idx * sizeof(OhciTd));
}

static uint8_t ohci_td_cc(const OhciTd *td) {
    return (uint8_t)((td->control >> OHCI_TD_CC_SHIFT) & 0x0Fu);
}

static int ohci_cc_ok(uint8_t cc) {
    return (cc == OHCI_CC_NO_ERROR || cc == OHCI_CC_DATA_UNDERRUN) ? 1 : 0;
}

static void ohci_td_setup(OhciTd *td, uint32_t ctrl, uint32_t buf_phys,
                          uint16_t len, uint32_t next_phys) {
    td->control = ctrl | OHCI_TD_DI_NONE | OHCI_TD_CC_NOT_ACC;
    td->cbp = (len && buf_phys) ? buf_phys : 0;
    td->nexttd = next_phys;
    td->be = (len && buf_phys) ? (buf_phys + (uint32_t)len - 1u) : 0;
}

static int ohci_alloc_pages(void) {
    g_kbd.ohci_hcca_page = pmm_alloc();
    g_kbd.ohci_ed_page = pmm_alloc();
    g_kbd.ohci_td_page = pmm_alloc();
    g_kbd.ohci_setup_page = pmm_alloc();
    g_kbd.data_buf_page = pmm_alloc();
    if (!g_kbd.ohci_hcca_page || !g_kbd.ohci_ed_page || !g_kbd.ohci_td_page ||
        !g_kbd.ohci_setup_page || !g_kbd.data_buf_page) {
        return 0;
    }
    kmemset((void *)(uintptr_t)g_kbd.ohci_hcca_page, 0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.ohci_ed_page, 0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.ohci_td_page, 0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.ohci_setup_page, 0, PAGE_SIZE);
    kmemset((void *)(uintptr_t)g_kbd.data_buf_page, 0, PAGE_SIZE);
    return 1;
}

static int ohci_controller_start(const UsbHostControllerInfo *info) {
    uint32_t saved_fm;
    uint32_t fi;
    uint32_t ctl;

    if (!info || !info->mmio_base || info->mmio_base > 0xFFFFFFFFull) return 0;
    g_kbd.ohci_mmio_base = (uint32_t)info->mmio_base;

    saved_fm = ohci_r32(OHCI_FM_INTERVAL);
    ctl = ohci_r32(OHCI_CONTROL);
    if (ctl & OHCI_CTL_IR) {
        ohci_w32(OHCI_CMD_STATUS, OHCI_CMD_OCR);
        for (int i = 0; i < 200; ++i) {
            xhci_delay(DELAY_1MS);
            if (!(ohci_r32(OHCI_CONTROL) & OHCI_CTL_IR)) break;
        }
    }

    ohci_w32(OHCI_INT_DISABLE, 0xFFFFFFFFu);
    ohci_w32(OHCI_CMD_STATUS, OHCI_CMD_HCR);
    for (int i = 0; i < 200; ++i) {
        xhci_delay(DELAY_1MS);
        if (!(ohci_r32(OHCI_CMD_STATUS) & OHCI_CMD_HCR)) break;
    }

    fi = saved_fm & 0x3FFFu;
    if (fi == 0 || fi == 0x3FFFu) fi = 0x2EDFu; /* 11999, USB 1ms frame */
    if ((saved_fm & 0x7FFF0000u) == 0) {
        uint32_t fsmps = (uint32_t)((6u * (fi - 210u)) / 7u);
        saved_fm = (fsmps << 16) | fi;
    }

    ohci_w32(OHCI_HCCA, g_kbd.ohci_hcca_page);
    ohci_w32(OHCI_CTRL_HEAD_ED, 0);
    ohci_w32(OHCI_CTRL_CUR_ED, 0);
    ohci_w32(OHCI_PERIOD_CUR_ED, 0);
    ohci_w32(OHCI_FM_INTERVAL, saved_fm);
    ohci_w32(OHCI_PERIODIC_START, (fi * 9u) / 10u);
    ohci_w32(OHCI_LS_THRESHOLD, 0x0628u);
    ohci_w32(OHCI_INT_STATUS, 0xFFFFFFFFu);
    ohci_w32(OHCI_INT_ENABLE, OHCI_INT_MIE | OHCI_INT_WDH);

    ctl = (3u) | OHCI_CTL_HCFS_OP | OHCI_CTL_CLE;
    ohci_w32(OHCI_CONTROL, ctl);
    xhci_delay(DELAY_100MS);

    /* Allumer le root hub. Certains controles ont du power global, d'autres par port. */
    ohci_w32(OHCI_RH_STATUS, OHCI_RH_STATUS_LPSC);
    xhci_delay(DELAY_100MS);
    return 1;
}

static int ohci_reset_port(uint8_t port_idx) {
    uint32_t paddr = ohci_port_status_addr(port_idx);
    uint32_t ps = mmio_r32(paddr);

    if (!(ps & OHCI_RH_PORT_CCS)) return 0;

    mmio_w32(paddr, OHCI_RH_PORT_PPS);
    xhci_delay(DELAY_100MS);
    ps = mmio_r32(paddr);
    if (ps & OHCI_RH_PORT_W1C) {
        mmio_w32(paddr, ps & OHCI_RH_PORT_W1C);
    }

    mmio_w32(paddr, OHCI_RH_PORT_PRS);
    for (int i = 0; i < 400; ++i) {
        xhci_delay(DELAY_1MS);
        ps = mmio_r32(paddr);
        if (!(ps & OHCI_RH_PORT_CCS)) return 0;
        if ((ps & OHCI_RH_PORT_PRSC) || ((ps & OHCI_RH_PORT_PES) && !(ps & OHCI_RH_PORT_PRS))) {
            if (ps & OHCI_RH_PORT_W1C) {
                mmio_w32(paddr, ps & OHCI_RH_PORT_W1C);
            }
            ps = mmio_r32(paddr);
            g_kbd.ohci_low_speed = (ps & OHCI_RH_PORT_LSDA) ? 1u : 0u;
            return (ps & OHCI_RH_PORT_PES) ? 1 : 0;
        }
    }

    kprintf("usb_hid: OHCI port reset timeout port=%u raw=%08x\n",
            (unsigned)(port_idx + 1u),
            (unsigned)mmio_r32(paddr));
    return 0;
}

static int ohci_control_transfer(uint8_t addr, uint8_t low_speed, uint16_t ep0_mps,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                 uint32_t data_phys) {
    OhciEd *ed = ohci_ctrl_ed();
    OhciTd *td0 = ohci_td(0);
    uint8_t *setup = (uint8_t *)(uintptr_t)g_kbd.ohci_setup_page;
    uint8_t idx = 0;
    uint8_t last_idx;
    uint32_t next_phys;

    kmemset((void *)(uintptr_t)g_kbd.ohci_td_page, 0, PAGE_SIZE);
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)(wValue & 0xFFu);
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)(wIndex & 0xFFu);
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)(wLength & 0xFFu);
    setup[7] = (uint8_t)(wLength >> 8);

    next_phys = ohci_td_phys(1);
    ohci_td_setup(td0, OHCI_TD_DP_SETUP | OHCI_TD_T_DATA0,
                  g_kbd.ohci_setup_page, 8u, next_phys);
    idx = 1;

    if (wLength > 0 && data_phys) {
        uint32_t dir = (bmRequestType & 0x80u) ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT;
        ohci_td_setup(ohci_td(idx), dir | OHCI_TD_T_DATA1,
                      data_phys, wLength, ohci_td_phys((uint8_t)(idx + 1u)));
        idx++;
    }

    {
        uint32_t status_dir = (wLength == 0 || !(bmRequestType & 0x80u)) ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT;
        ohci_td_setup(ohci_td(idx), status_dir | OHCI_TD_T_DATA1,
                      0, 0, ohci_td_phys((uint8_t)(idx + 1u)));
        last_idx = idx;
        idx++;
    }

    /* TD factice en queue. */
    ohci_td(idx)->control = 0;
    ohci_td(idx)->cbp = 0;
    ohci_td(idx)->nexttd = 0;
    ohci_td(idx)->be = 0;

    ed->control = OHCI_ED_FA(addr) | OHCI_ED_EN(0) |
                  (low_speed ? OHCI_ED_LOW_SPEED : 0u) |
                  OHCI_ED_MPS(ep0_mps ? ep0_mps : 8u);
    ed->tailp = ohci_td_phys(idx);
    ed->headp = ohci_td_phys(0);
    ed->nexted = 0;

    ohci_w32(OHCI_CTRL_HEAD_ED, g_kbd.ohci_ed_page);
    ohci_w32(OHCI_CTRL_CUR_ED, 0);
    ohci_w32(OHCI_INT_STATUS, OHCI_INT_WDH);
    ohci_w32(OHCI_CONTROL, (ohci_r32(OHCI_CONTROL) | OHCI_CTL_CLE | OHCI_CTL_HCFS_OP));
    ohci_w32(OHCI_CMD_STATUS, OHCI_CMD_CLF);

    for (uint32_t t = 0; t < 1500000u; ++t) {
        uint8_t cc = ohci_td_cc(ohci_td(last_idx));
        if (cc != 0x0Fu) {
            ohci_w32(OHCI_INT_STATUS, OHCI_INT_WDH);
            if (!ohci_cc_ok(cc)) {
                kprintf("usb_hid: OHCI ctrl cc=%u req=%u len=%u\n",
                        (unsigned)cc,
                        (unsigned)bRequest,
                        (unsigned)wLength);
                return 0;
            }
            return 1;
        }
        if (ed->headp & 0x1u) {
            kprintf("usb_hid: OHCI ctrl halted req=%u head=%08x\n",
                    (unsigned)bRequest,
                    (unsigned)ed->headp);
            return 0;
        }
        __asm__ volatile ("pause");
    }

    kprintf("usb_hid: OHCI ctrl timeout req=%u len=%u\n",
            (unsigned)bRequest,
            (unsigned)wLength);
    return 0;
}

static void ohci_submit_intr_in(void) {
    OhciEd *ed = ohci_intr_ed();
    uint8_t td_idx = 8u;
    uint8_t tail_idx = 9u;
    uint16_t mps = (g_kbd.ep1_mps > 0 && g_kbd.ep1_mps <= 8u) ? g_kbd.ep1_mps : 8u;
    uint32_t toggle = g_kbd.ohci_intr_toggle ? OHCI_TD_T_DATA1 : OHCI_TD_T_DATA0;

    if (g_kbd.ohci_intr_pending) return;

    ed->control |= OHCI_ED_SKIP;
    ohci_td_setup(ohci_td(td_idx), OHCI_TD_DP_IN | toggle,
                  g_kbd.data_buf_page, mps, ohci_td_phys(tail_idx));
    ohci_td(tail_idx)->control = 0;
    ohci_td(tail_idx)->cbp = 0;
    ohci_td(tail_idx)->nexttd = 0;
    ohci_td(tail_idx)->be = 0;
    ed->tailp = ohci_td_phys(tail_idx);
    ed->headp = ohci_td_phys(td_idx);
    ed->control &= ~OHCI_ED_SKIP;

    g_kbd.ohci_intr_pending = 1u;
}

static void ohci_setup_interrupt_endpoint(void) {
    OhciHcca *hcca = (OhciHcca *)(uintptr_t)g_kbd.ohci_hcca_page;
    OhciEd *ed = ohci_intr_ed();
    uint8_t ep = (uint8_t)(g_kbd.ep1_addr & 0x0Fu);
    uint16_t mps = (g_kbd.ep1_mps > 0 && g_kbd.ep1_mps <= 8u) ? g_kbd.ep1_mps : 8u;

    ed->control = OHCI_ED_FA(g_kbd.ohci_addr) | OHCI_ED_EN(ep) | OHCI_ED_DIR_IN |
                  (g_kbd.ohci_low_speed ? OHCI_ED_LOW_SPEED : 0u) |
                  OHCI_ED_MPS(mps);
    ed->headp = ohci_td_phys(9u);
    ed->tailp = ohci_td_phys(9u);
    ed->nexted = 0;

    for (int i = 0; i < 32; ++i) {
        hcca->int_table[i] = g_kbd.ohci_ed_page + sizeof(OhciEd);
    }

    ohci_w32(OHCI_CONTROL, ohci_r32(OHCI_CONTROL) | OHCI_CTL_PLE | OHCI_CTL_HCFS_OP);
    g_kbd.ohci_intr_toggle = 0u;
    g_kbd.ohci_intr_pending = 0u;
    ohci_submit_intr_in();
}

static void ohci_kbd_poll(void) {
    if (!g_kbd.ohci_intr_pending) {
        ohci_submit_intr_in();
        return;
    }

    {
        uint8_t td_idx = 8u;
        uint8_t cc = ohci_td_cc(ohci_td(td_idx));
        if (cc == 0x0Fu) return;

        g_kbd.ohci_intr_pending = 0u;
        ohci_w32(OHCI_INT_STATUS, OHCI_INT_WDH);
        if (ohci_cc_ok(cc)) {
            const uint8_t *report = (const uint8_t *)(uintptr_t)g_kbd.data_buf_page;
            process_hid_report(report);
            g_kbd.ohci_intr_toggle ^= 1u;
        }
        ohci_submit_intr_in();
    }
}

static int ohci_try_port(const UsbHostControllerInfo *info, uint8_t port_idx) {
    uint16_t ep0_mps = 8u;
    uint8_t cfg_val;
    uint8_t *buf = (uint8_t *)(uintptr_t)g_kbd.data_buf_page;

    g_kbd.port_idx = port_idx;
    if (!ohci_reset_port(port_idx)) {
        return 0;
    }

    kprintf("usb_hid: OHCI essai port=%u low=%u\n",
            (unsigned)(port_idx + 1u),
            (unsigned)g_kbd.ohci_low_speed);

    if (!ohci_control_transfer(0, g_kbd.ohci_low_speed, ep0_mps,
                               RT_DEV_TO_HOST_STD_DEV, USB_REQ_GET_DESCRIPTOR,
                               USB_DESC_DEVICE, 0, 8u, g_kbd.data_buf_page)) {
        return 0;
    }
    ep0_mps = ep0_mps_from_device_desc(g_kbd.ohci_low_speed ? SPEED_LOW : SPEED_FULL, buf[7]);

    if (!ohci_control_transfer(0, g_kbd.ohci_low_speed, ep0_mps,
                               RT_HOST_TO_DEV_STD_DEV, 0x05u,
                               1u, 0, 0, 0)) {
        return 0;
    }
    g_kbd.ohci_addr = 1u;
    xhci_delay(DELAY_10MS);

    if (!ohci_control_transfer(g_kbd.ohci_addr, g_kbd.ohci_low_speed, ep0_mps,
                               RT_DEV_TO_HOST_STD_DEV, USB_REQ_GET_DESCRIPTOR,
                               USB_DESC_DEVICE, 0, 18u, g_kbd.data_buf_page)) {
        return 0;
    }

    if (!ohci_control_transfer(g_kbd.ohci_addr, g_kbd.ohci_low_speed, ep0_mps,
                               RT_DEV_TO_HOST_STD_DEV, USB_REQ_GET_DESCRIPTOR,
                               USB_DESC_CONFIG, 0, 255u, g_kbd.data_buf_page)) {
        return 0;
    }
    {
        uint16_t total = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
        if (total > 255u) total = 255u;
        if (!parse_config_descriptor(buf, total)) {
            kprintf("usb_hid: OHCI no HID boot keyboard on port=%u\n",
                    (unsigned)(port_idx + 1u));
            return 0;
        }
    }

    cfg_val = buf[5];
    if (!cfg_val) cfg_val = 1u;
    if (!ohci_control_transfer(g_kbd.ohci_addr, g_kbd.ohci_low_speed, ep0_mps,
                               RT_HOST_TO_DEV_STD_DEV, USB_REQ_SET_CONFIGURATION,
                               cfg_val, 0, 0, 0)) {
        return 0;
    }

    ohci_control_transfer(g_kbd.ohci_addr, g_kbd.ohci_low_speed, ep0_mps,
                          RT_HOST_TO_DEV_CLS_IF, HID_REQ_SET_PROTOCOL,
                          HID_BOOT_PROTOCOL, g_kbd.interface_num, 0, 0);
    ohci_control_transfer(g_kbd.ohci_addr, g_kbd.ohci_low_speed, ep0_mps,
                          RT_HOST_TO_DEV_CLS_IF, HID_REQ_SET_IDLE,
                          0, g_kbd.interface_num, 0, 0);

    kmemset(g_kbd.last_report, 0, sizeof(g_kbd.last_report));
    g_kbd.backend = 2u;
    g_kbd.present = 1;
    ohci_setup_interrupt_endpoint();
    kprintf("usb_hid: clavier OHCI OK b%u s%u port=%u addr=%u ep=0x%x mps=%u\n",
            (unsigned)info->bus,
            (unsigned)info->slot,
            (unsigned)(port_idx + 1u),
            (unsigned)g_kbd.ohci_addr,
            (unsigned)g_kbd.ep1_addr,
            (unsigned)g_kbd.ep1_mps);
    return 1;
}

static int ohci_keyboard_init(void) {
    int count = usb_probe_count();
    int seen = 0;
    int connected = 0;

    for (int i = 0; i < count; ++i) {
        const UsbHostControllerInfo *cur = usb_probe_get(i);
        uint8_t ports;
        if (!cur || cur->kind != USB_HC_OHCI ||
            !cur->mmio_base || cur->mmio_base > 0xFFFFFFFFull) {
            continue;
        }
        seen = 1;
        kprintf("usb_hid: OHCI candidate b%u s%u f%u mmio=%x\n",
                (unsigned)cur->bus,
                (unsigned)cur->slot,
                (unsigned)cur->func,
                (unsigned)(uint32_t)cur->mmio_base);

        kmemset(&g_kbd, 0, sizeof(g_kbd));
        if (!ohci_alloc_pages()) {
            kprintf("usb_hid: OHCI OOM\n");
            return 0;
        }
        if (!ohci_controller_start(cur)) {
            kprintf("usb_hid: OHCI start failed\n");
            continue;
        }

        ports = cur->port_count;
        if (!ports) {
            ports = (uint8_t)(ohci_r32(OHCI_RH_DESC_A) & 0xFFu);
        }
        if (ports > 16u) ports = 16u;

        kprintf("usb_hid: OHCI b%u s%u ports=%u rev=%x\n",
                (unsigned)cur->bus,
                (unsigned)cur->slot,
                (unsigned)ports,
                (unsigned)ohci_r32(OHCI_REVISION));

        for (uint8_t port = 0; port < ports; ++port) {
            uint32_t ps = mmio_r32(ohci_port_status_addr(port));
            if (!(ps & OHCI_RH_PORT_CCS)) continue;
            connected = 1;
            kprintf("usb_hid: OHCI port=%u raw=%08x\n",
                    (unsigned)(port + 1u),
                    (unsigned)ps);
            if (ohci_try_port(cur, port)) {
                return 1;
            }
        }
    }

    if (!seen) kprintf("usb_hid: OHCI absent du probe PCI\n");
    else if (!connected) kprintf("usb_hid: OHCI aucun port connecte\n");
    return 0;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * POINT D'ENTRÃ‰E : usb_hid_kbd_init()
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

int usb_hid_kbd_init(void) {
    const UsbHostControllerInfo *info = 0;
    int count = usb_probe_count();

    kmemset(&g_kbd, 0, sizeof(g_kbd));
    log_usb_hc_summary();

    /* Sur certaines cartes AMD/Asus, les claviers full-speed restent
       routes vers le compagnon OHCI. On l'essaie avant de secouer le xHCI. */
    if (ohci_keyboard_init()) {
        return 1;
    }
    kmemset(&g_kbd, 0, sizeof(g_kbd));

    /* Si le compagnon OHCI ne repond pas, on prend la main sur xHCI/EHCI
       seulement maintenant. Ca evite de derouter trop tot les claviers USB1. */
    usb_probe_take_ownership();

    /* 1. Trouver le xHCI */
    for (int i = 0; i < count; ++i) {
        const UsbHostControllerInfo *cur = usb_probe_get(i);
        if (cur && cur->kind == USB_HC_XHCI &&
            cur->mmio_base && cur->mmio_base <= 0xFFFFFFFFull &&
            cur->legacy_handoff_ok) {
            info = cur;
            break;
        }
    }
    if (!info) {
        return ohci_keyboard_init();
    }

    g_kbd.mmio_base = (uint32_t)info->mmio_base;
    g_kbd.op_base   = g_kbd.mmio_base + (uint32_t)info->cap_length;
    g_kbd.max_slots = info->slot_count ? info->slot_count : 1u;
    {
        uint32_t hccparams1 = mmio_r32(g_kbd.mmio_base + XHCI_HCCPARAMS1_REG);
        g_kbd.ctx_size = (hccparams1 & (1u << 2)) ? 64u : 32u;
        kprintf("usb_hid: xhci csz=%u\n", (unsigned)g_kbd.ctx_size);
    }

    /* 2. Initialiser les rings xHCI */
    if (!xhci_init_rings()) {
        kprintf("usb_hid: OOM rings\n");
        if (ohci_keyboard_init()) return 1;
        g_kbd.error = 1;
        return 0;
    }

    /* Laisser au xHC et aux ports le temps de se stabiliser apres reset global. */
    xhci_delay(DELAY_100MS);
    evt_drain(EVT_RING_TRBS * 2u);

    /* 3..6. Essayer chaque port connecte. On privilegie les ports USB2
       (clavier probable) avant les ports SuperSpeed (souvent la cle de boot). */
    {
        int have_connected_port = 0;

        for (int pass = 0; pass < 2; ++pass) {
            for (uint8_t port = 0; port < info->port_count && port < 16u; ++port) {
                uint8_t spd = 0;
                uint16_t learned_mps;

                if (!port_connected(port, &spd)) continue;
                if (port_try_priority(spd) != pass) continue;
                have_connected_port = 1;

                g_kbd.port_idx = port;
                g_kbd.speed = spd;
                reset_attempt_state();

                kprintf("usb_hid: essai port=%u speed=%u\n",
                        (unsigned)(port + 1u),
                        (unsigned)spd);

                if (!port_reset(g_kbd.port_idx)) {
                    uint32_t ps = xhci_portsc_read(g_kbd.port_idx);
                    xhci_log_portsc("reset timeout", g_kbd.port_idx, ps);
                    if (!xhci_port_ready(g_kbd.port_idx)) {
                        continue; /* Pas d'Address Device sur un port non-enable. */
                    }
                } else {
                    xhci_delay(DELAY_10MS); /* Delai post-reset USB 2.0 (spec: >=10 ms) */
                }

                g_kbd.slot_id = xhci_enable_slot();
                if (!g_kbd.slot_id) {
                    continue;
                }

                dcbaap_set_slot_ctx(g_kbd.slot_id, g_kbd.dev_ctx_page);

                learned_mps = ep0_max_packet_size(g_kbd.speed);
                setup_input_context_ep0(learned_mps);
                if (!xhci_address_device(g_kbd.slot_id, g_kbd.input_ctx_page, 0)) {
                    kprintf("usb_hid: Address Device BSR=0 failed\n");
                    continue;
                }
                kprintf("usb_hid: Address Device OK port=%u mps=%u\n",
                        (unsigned)(g_kbd.port_idx + 1u),
                        (unsigned)learned_mps);

                goto device_addressed;
            }
        }

        if (!have_connected_port) {
            log_ehci_connected_ports();
            return ohci_keyboard_init();
        }

        log_ehci_connected_ports();
        if (ohci_keyboard_init()) {
            return 1;
        }
        g_kbd.error = 1;
        return 0;
    }

device_addressed:

    /* Lire l'adresse USB assignÃ©e depuis le Device Context */
    {
        const SlotCtx *sc = (const SlotCtx *)(uintptr_t)g_kbd.dev_ctx_page;
        g_kbd.dev_addr = (uint8_t)(sc->dw3 & 0xFFu);
    }

    /* 7. GET_DESCRIPTOR(Device) â€” pour valider et lire bMaxPacketSize0 */
    {
        uint8_t *buf = (uint8_t *)(uintptr_t)g_kbd.data_buf_page;
        if (!control_transfer(RT_DEV_TO_HOST_STD_DEV, USB_REQ_GET_DESCRIPTOR,
                              USB_DESC_DEVICE, 0, 18u,
                              g_kbd.data_buf_page)) {
            kprintf("usb_hid: GET_DESCRIPTOR Device failed\n");
            if (ohci_keyboard_init()) return 1;
            g_kbd.error = 1;
            return 0;
        }
        /* Optionnel : mettre Ã  jour MPS EP0 si nÃ©cessaire */
        (void)buf;
    }

    /* 8. GET_DESCRIPTOR(Configuration, wLength=255) */
    {
        if (!control_transfer(RT_DEV_TO_HOST_STD_DEV, USB_REQ_GET_DESCRIPTOR,
                              USB_DESC_CONFIG, 0, 255u,
                              g_kbd.data_buf_page)) {
            kprintf("usb_hid: GET_DESCRIPTOR Config failed\n");
            if (ohci_keyboard_init()) return 1;
            g_kbd.error = 1;
            return 0;
        }

        const uint8_t *buf = (const uint8_t *)(uintptr_t)g_kbd.data_buf_page;
        /* wTotalLength est Ã  l'offset 2 du Config Descriptor */
        uint16_t total = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
        if (total > 255u) total = 255u;

        if (!parse_config_descriptor(buf, total)) {
            kprintf("usb_hid: no HID boot keyboard interface found\n");
            /* Pas une erreur fatale du xHC, mais pas un clavier */
            return ohci_keyboard_init();
        }
    }

    /* 9. SET_CONFIGURATION(1) */
    {
        const uint8_t *buf = (const uint8_t *)(uintptr_t)g_kbd.data_buf_page;
        uint8_t cfg_val = buf[5]; /* bConfigurationValue */
        if (!cfg_val) cfg_val = 1u;
        if (!control_transfer(RT_HOST_TO_DEV_STD_DEV, USB_REQ_SET_CONFIGURATION,
                              (uint16_t)cfg_val, 0, 0, 0)) {
            kprintf("usb_hid: SET_CONFIGURATION failed\n");
            if (ohci_keyboard_init()) return 1;
            g_kbd.error = 1;
            return 0;
        }
    }

    /* 10. Configure Endpoint : ajouter EP1 IN dans le Device Context */
    setup_input_context_ep1();
    if (!xhci_configure_ep(g_kbd.slot_id, g_kbd.input_ctx_page)) {
        kprintf("usb_hid: Configure Endpoint failed\n");
        if (ohci_keyboard_init()) return 1;
        g_kbd.error = 1;
        return 0;
    }

    /* 11. SET_PROTOCOL(Boot) â€” HID class request */
    if (!control_transfer(RT_HOST_TO_DEV_CLS_IF, HID_REQ_SET_PROTOCOL,
                          HID_BOOT_PROTOCOL,
                          (uint16_t)g_kbd.interface_num, 0, 0)) {
        kprintf("usb_hid: SET_PROTOCOL failed (non fatal)\n");
        /* Non fatal â€” certains claviers acceptent quand mÃªme */
    }

    /* 12. SET_IDLE(0, 0) â€” Pas de rapport rÃ©pÃ©tÃ© */
    control_transfer(RT_HOST_TO_DEV_CLS_IF, HID_REQ_SET_IDLE,
                     0x0000u, (uint16_t)g_kbd.interface_num, 0, 0);
    /* IgnorÃ© si refusÃ© */

    /* ZÃ©roÃ¯ser le last_report */
    kmemset(g_kbd.last_report, 0, sizeof(g_kbd.last_report));

    /* Soumettre le premier TD sur EP1 IN */
    ep1_submit(g_kbd.data_buf_page);

    g_kbd.backend = 1u;
    g_kbd.present = 1;

    kprintf("usb_hid: clavier OK slot=%u port=%u addr=%u ep=0x%x mps=%u\n",
            (unsigned)g_kbd.slot_id,
            (unsigned)g_kbd.port_idx,
            (unsigned)g_kbd.dev_addr,
            (unsigned)g_kbd.ep1_addr,
            (unsigned)g_kbd.ep1_mps);

    return 1;
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * usb_hid_kbd_poll() â€” appelÃ© dans la boucle shell
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

void usb_hid_kbd_poll(void) {
    if (!g_kbd.present || g_kbd.error) return;

    if (g_kbd.backend == 2u) {
        ohci_kbd_poll();
        return;
    }
    if (g_kbd.backend != 1u) return;

    if (ep1_check_event()) {
        /* Un rapport est arrivÃ© dans data_buf_page */
        const uint8_t *report = (const uint8_t *)(uintptr_t)g_kbd.data_buf_page;
        process_hid_report(report);
        /* Remettre un TD en attente */
        ep1_submit(g_kbd.data_buf_page);
    }
}

/* â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
 * API publique
 * â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â• */

int usb_hid_kbd_present(void) {
    return g_kbd.present;
}

void usb_hid_kbd_status_string(char *buf, uint32_t bufsz) {
    if (!buf || !bufsz) return;
    if (!g_kbd.present) {
        kstrncpy(buf, g_kbd.error ? "usb_hid: erreur init" : "usb_hid: absent", bufsz - 1u);
    } else if (g_kbd.backend == 2u) {
        ksprintf(buf, "usb_hid: ok ohci port=%u addr=%u ep=0x%x mps=%u",
                 (unsigned)(g_kbd.port_idx + 1u),
                 (unsigned)g_kbd.ohci_addr,
                 (unsigned)g_kbd.ep1_addr,
                 (unsigned)g_kbd.ep1_mps);
    } else {
        ksprintf(buf, "usb_hid: ok slot=%u port=%u ep=0x%x mps=%u",
                 (unsigned)g_kbd.slot_id,
                 (unsigned)(g_kbd.port_idx + 1u),
                 (unsigned)g_kbd.ep1_addr,
                 (unsigned)g_kbd.ep1_mps);
    }
    buf[bufsz - 1u] = '\0';
}


